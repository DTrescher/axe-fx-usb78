#pragma once

#include "axe/stereo_spsc_ring.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <limits>
#include <stdexcept>

namespace axe {

struct AudioPipelineConfig {
    std::uint32_t input_rate_hz{48'000};
    std::uint32_t output_rate_hz{48'000};
    std::size_t capacity_frames{8'192};
    std::size_t target_fill_frames{1'024};
    std::size_t preroll_frames{256};
    // If queued audio exceeds this bound, the render thread drops the oldest
    // frames back to target_fill_frames before producing output.
    std::size_t max_latency_frames{4'096};
    std::int32_t max_correction_ppm{1'500};
};

struct AudioPipelineStats {
    std::uint64_t input_frames_accepted{};
    std::uint64_t overflow_frames{};
    std::uint64_t dropout_frames{};
    std::uint64_t stale_frames_discarded{};
    std::uint64_t non_finite_samples_sanitized{};
    std::uint64_t discontinuities{};
    std::uint32_t buffered_frames{};
    std::int32_t correction_ppm{};
    bool priming{};
};

// A fixed-memory bridge between a WASAPI producer and an ASIO consumer.
// Construct it before starting those threads. `ingest`, `submit_silence`, and
// `notify_discontinuity` belong to one producer thread; `render` belongs to one
// consumer thread. Stats may be read by any thread.
//
// The render side performs only bounded loops and lock-free atomic operations.
// No reset operation writes the other thread's cursor: discontinuities request
// a flush which the render thread performs itself.
class AudioPipeline final {
public:
    static constexpr std::size_t filter_taps = 32;
    static constexpr std::size_t filter_phases = 1'024;

    explicit AudioPipeline(AudioPipelineConfig config = {})
        : config_(validate(config)), ring_(config_.capacity_frames),
          nominal_step_(static_cast<double>(config_.input_rate_hz) /
                        static_cast<double>(config_.output_rate_hz)),
          filter_coefficients_(new float[filter_taps * filter_phases]), priming_(true) {
        build_filter();
    }

    AudioPipeline(const AudioPipeline&) = delete;
    AudioPipeline& operator=(const AudioPipeline&) = delete;
    AudioPipeline(AudioPipeline&&) = delete;
    AudioPipeline& operator=(AudioPipeline&&) = delete;

    // Producer thread. Non-finite samples become silence. If the bounded ring
    // is full, the newest frames are rejected so the producer never changes
    // the consumer cursor.
    [[nodiscard]] std::uint32_t ingest(const float* interleaved,
                                       std::uint32_t frames) noexcept {
        if (interleaved == nullptr || frames == 0) {
            return 0;
        }
        std::uint64_t sanitized = 0;
        const std::size_t accepted = ring_.push_mapped(
            interleaved, frames, [&sanitized](float left, float right) noexcept {
                if (!std::isfinite(left)) {
                    left = 0.0f;
                    ++sanitized;
                }
                if (!std::isfinite(right)) {
                    right = 0.0f;
                    ++sanitized;
                }
                return StereoFrame{left, right};
            });
        input_frames_accepted_.fetch_add(accepted, std::memory_order_relaxed);
        overflow_frames_.fetch_add(static_cast<std::uint64_t>(frames) - accepted,
                                   std::memory_order_relaxed);
        non_finite_samples_sanitized_.fetch_add(sanitized, std::memory_order_relaxed);
        return static_cast<std::uint32_t>(accepted);
    }

    // Producer thread. Inserts bounded zeros while preserving the source
    // timeline, useful for WASAPI packets marked silent.
    [[nodiscard]] std::uint32_t submit_silence(std::uint32_t frames) noexcept {
        if (frames == 0) {
            return 0;
        }
        const std::size_t accepted = ring_.push_silence(frames);
        input_frames_accepted_.fetch_add(accepted, std::memory_order_relaxed);
        overflow_frames_.fetch_add(static_cast<std::uint64_t>(frames) - accepted,
                                   std::memory_order_relaxed);
        return static_cast<std::uint32_t>(accepted);
    }

    // Producer thread. The render thread consumes this request, flushes queued
    // pre-boundary audio and resets interpolation. Frames submitted after the
    // boundary remain queued and can restart an already-running stream promptly.
    void notify_discontinuity() noexcept {
        discontinuity_boundary_.store(ring_.producer_position(), std::memory_order_relaxed);
        discontinuity_requested_.store(true, std::memory_order_release);
    }

    // Consumer / ASIO callback thread. Output is planar stereo. Both pointers
    // must refer to `frames` writable floats. All output samples are assigned,
    // including silence on underrun or while prerolling.
    void render(float* left, float* right, std::uint32_t frames) noexcept {
        if (left == nullptr || right == nullptr || frames == 0) {
            return;
        }

        auto cursor = ring_.begin_consumer_read();
        std::uint64_t discontinuities = 0;
        std::uint64_t stale_drops = 0;
        std::uint64_t dropouts = 0;
        bool priming = priming_.load(std::memory_order_relaxed);
        if (discontinuity_requested_.exchange(false, std::memory_order_acq_rel)) {
            cursor.refresh();
            const std::uint64_t boundary =
                discontinuity_boundary_.load(std::memory_order_relaxed);
            stale_drops += cursor.discard_before(boundary);
            phase_ = 0.0;
            filtered_occupancy_error_ = 0.0;
            reset_history();
            ++discontinuities;
        }

        if (cursor.available() > config_.max_latency_frames) {
            const std::size_t discard = cursor.available() - config_.target_fill_frames;
            cursor.consume(discard);
            phase_ = 0.0;
            filtered_occupancy_error_ = 0.0;
            reset_history();
            priming = true;
            stale_drops += discard;
            ++discontinuities;
        }

        const bool skip_correction_this_block = priming;
        if (priming) {
            if (cursor.available() < config_.preroll_frames) {
                cursor.refresh();
            }
            if (cursor.available() < config_.preroll_frames) {
                std::fill_n(left, frames, 0.0f);
                std::fill_n(right, frames, 0.0f);
                cursor.commit();
                priming_.store(true, std::memory_order_release);
                dropout_frames_.fetch_add(frames, std::memory_order_relaxed);
                if (stale_drops != 0) {
                    stale_frames_discarded_.fetch_add(stale_drops, std::memory_order_relaxed);
                }
                if (discontinuities != 0) {
                    discontinuities_.fetch_add(discontinuities, std::memory_order_relaxed);
                }
                return;
            }
            phase_ = 0.0;
            priming = false;
        }

        update_clock_correction(frames, cursor.available(), skip_correction_this_block);
        const std::int32_t correction = correction_ppm_.load(std::memory_order_relaxed);
        const double step = nominal_step_ *
                            (1.0 + static_cast<double>(correction) * 1.0e-6);
        const bool unity_fast_path = nominal_step_ == 1.0 && correction == 0;

        for (std::uint32_t out = 0; out < frames; ++out) {
            const double next_phase = phase_ + step;
            const auto consume_count = static_cast<std::size_t>(next_phase);
            const std::size_t lookahead = unity_fast_path ? 1 : filter_taps / 2 + 1;
            const std::size_t needed = std::max(consume_count, lookahead);
            if (cursor.available() < needed) {
                cursor.refresh();
            }

            StereoFrame first{};
            if (cursor.available() < needed || !cursor.peek(0, first)) {
                left[out] = 0.0f;
                right[out] = 0.0f;
                dropouts += frames - out;
                if (!priming) {
                    ++discontinuities;
                }
                priming = true;
                const std::size_t stranded = cursor.available();
                cursor.consume(stranded);
                stale_drops += stranded;
                phase_ = 0.0;
                filtered_occupancy_error_ = 0.0;
                reset_history();
                for (std::uint32_t rest = out + 1; rest < frames; ++rest) {
                    left[rest] = 0.0f;
                    right[rest] = 0.0f;
                }
                break;
            }

            if (unity_fast_path) {
                left[out] = first.left;
                right[out] = first.right;
            } else {
                const std::size_t phase_index = std::min<std::size_t>(
                    static_cast<std::size_t>(phase_ * filter_phases), filter_phases - 1);
                const float* coefficients = filter_coefficients_.get() + phase_index * filter_taps;
                double sum_left = 0.0;
                double sum_right = 0.0;
                for (std::size_t tap = 0; tap < filter_taps; ++tap) {
                    const int offset = static_cast<int>(tap) -
                                       static_cast<int>(kFilterPastFrames);
                    StereoFrame sample{};
                    if (offset < 0) {
                        const auto back = static_cast<std::size_t>(-offset);
                        if (back <= history_valid_) {
                            const std::size_t index =
                                (history_write_ + kFilterPastFrames - back) % kFilterPastFrames;
                            sample = history_[index];
                        }
                    } else if (!cursor.peek(static_cast<std::size_t>(offset), sample)) {
                        // The lookahead check above guarantees this for the
                        // current producer snapshot. Keep the output finite if
                        // a caller violates the SPSC contract.
                        sample = {};
                    }
                    const double coefficient = coefficients[tap];
                    sum_left += static_cast<double>(sample.left) * coefficient;
                    sum_right += static_cast<double>(sample.right) * coefficient;
                }
                left[out] = bounded_output(sum_left);
                right[out] = bounded_output(sum_right);
            }

            for (std::size_t consumed = 0; consumed < consume_count; ++consumed) {
                StereoFrame consumed_frame{};
                if (cursor.peek(consumed, consumed_frame)) {
                    remember(consumed_frame);
                }
            }
            phase_ = next_phase - static_cast<double>(consume_count);
            if (consume_count != 0) {
                cursor.consume(consume_count);
            }
        }

        cursor.commit();
        priming_.store(priming, std::memory_order_release);
        if (dropouts != 0) {
            dropout_frames_.fetch_add(dropouts, std::memory_order_relaxed);
        }
        if (stale_drops != 0) {
            stale_frames_discarded_.fetch_add(stale_drops, std::memory_order_relaxed);
        }
        if (discontinuities != 0) {
            discontinuities_.fetch_add(discontinuities, std::memory_order_relaxed);
        }
    }

    [[nodiscard]] AudioPipelineStats stats() const noexcept {
        return AudioPipelineStats{
            input_frames_accepted_.load(std::memory_order_relaxed),
            overflow_frames_.load(std::memory_order_relaxed),
            dropout_frames_.load(std::memory_order_relaxed),
            stale_frames_discarded_.load(std::memory_order_relaxed),
            non_finite_samples_sanitized_.load(std::memory_order_relaxed),
            discontinuities_.load(std::memory_order_relaxed),
            static_cast<std::uint32_t>(ring_.available()),
            correction_ppm_.load(std::memory_order_relaxed),
            priming_.load(std::memory_order_relaxed)};
    }

private:
    static AudioPipelineConfig validate(AudioPipelineConfig config) {
        if (config.input_rate_hz == 0 || config.output_rate_hz == 0) {
            throw std::invalid_argument("audio sample rates must be nonzero");
        }
        if (config.capacity_frames < 2 || config.capacity_frames > 0x7fff'ffffu) {
            throw std::invalid_argument("audio ring capacity must be in [2, 2^31-1]");
        }
        if (config.target_fill_frames < 2 ||
            config.target_fill_frames >= config.capacity_frames) {
            throw std::invalid_argument("target fill must be in [2, capacity)");
        }
        if (config.preroll_frames < filter_taps / 2 + 1 ||
            config.preroll_frames >= config.capacity_frames) {
            throw std::invalid_argument("preroll must provide 17 frames of filter lookahead");
        }
        if (config.max_latency_frames < config.target_fill_frames ||
            config.max_latency_frames > config.capacity_frames) {
            throw std::invalid_argument("max latency must be between target fill and capacity");
        }
        if (config.max_correction_ppm < 0 || config.max_correction_ppm > 10'000) {
            throw std::invalid_argument("max correction must be in [0, 10000] ppm");
        }
        return config;
    }

    static constexpr std::size_t kFilterPastFrames = 15;

    void build_filter() noexcept {
        const double rate_ratio = std::min(
            1.0, static_cast<double>(config_.output_rate_hz) /
                     static_cast<double>(config_.input_rate_hz));
        const double cutoff = 0.96 * rate_ratio;
        constexpr double pi = 3.1415926535897932384626433832795;
        for (std::size_t phase = 0; phase < filter_phases; ++phase) {
            const double fraction = static_cast<double>(phase) / filter_phases;
            double sum = 0.0;
            for (std::size_t tap = 0; tap < filter_taps; ++tap) {
                const double offset = static_cast<double>(static_cast<int>(tap) -
                                                          static_cast<int>(kFilterPastFrames));
                const double distance = offset - fraction;
                const double scaled = cutoff * distance;
                const double sinc = std::abs(scaled) < 1.0e-12
                                        ? 1.0
                                        : std::sin(pi * scaled) / (pi * scaled);
                const double normalized = distance / 16.0;
                const double window = std::abs(normalized) >= 1.0
                                          ? 0.0
                                          : 0.42 + 0.5 * std::cos(pi * normalized) +
                                                0.08 * std::cos(2.0 * pi * normalized);
                const double coefficient = cutoff * sinc * window;
                filter_coefficients_[phase * filter_taps + tap] =
                    static_cast<float>(coefficient);
                sum += coefficient;
            }
            const float reciprocal = static_cast<float>(1.0 / sum);
            for (std::size_t tap = 0; tap < filter_taps; ++tap) {
                filter_coefficients_[phase * filter_taps + tap] *= reciprocal;
            }
        }
    }

    void remember(StereoFrame frame) noexcept {
        history_[history_write_] = frame;
        history_write_ = (history_write_ + 1) % kFilterPastFrames;
        history_valid_ = std::min(history_valid_ + 1, kFilterPastFrames);
    }

    static float bounded_output(double sample) noexcept {
        if (!std::isfinite(sample)) {
            return 0.0f;
        }
        constexpr double limit = std::numeric_limits<float>::max();
        if (sample > limit) {
            return std::numeric_limits<float>::max();
        }
        if (sample < -limit) {
            return -std::numeric_limits<float>::max();
        }
        return static_cast<float>(sample);
    }

    void reset_history() noexcept {
        history_.fill({});
        history_write_ = 0;
        history_valid_ = 0;
    }

    void update_clock_correction(std::uint32_t output_frames, std::size_t occupancy,
                                 bool priming) noexcept {
        if (priming || output_frames == 0) {
            return;
        }
        const double error = static_cast<double>(occupancy) -
                             static_cast<double>(config_.target_fill_frames);
        const double elapsed_seconds = static_cast<double>(output_frames) /
                                       static_cast<double>(config_.output_rate_hz);
        // Smooth occupancy over roughly half a second so 10 ms WASAPI packet
        // bursts do not drive ASIO's independent clock correction. Positive
        // correction consumes input faster when the queue's filtered level is
        // above target; negative correction slows it when below target.
        constexpr double proportional = 1.5e-6;
        constexpr double integral = 2.0e-10;
        constexpr double occupancy_time_constant_seconds = 0.5;
        const double alpha = elapsed_seconds /
                             (occupancy_time_constant_seconds + elapsed_seconds);
        filtered_occupancy_error_ += alpha * (error - filtered_occupancy_error_);
        integral_correction_ += filtered_occupancy_error_ * elapsed_seconds * integral;
        const double max_ratio = static_cast<double>(config_.max_correction_ppm) * 1.0e-6;
        integral_correction_ = std::clamp(integral_correction_, -max_ratio, max_ratio);
        const double ratio = std::clamp(proportional * filtered_occupancy_error_ +
                                            integral_correction_,
                                        -max_ratio, max_ratio);
        const auto ppm = static_cast<std::int32_t>(std::lround(ratio * 1.0e6));
        correction_ppm_.store(ppm, std::memory_order_relaxed);
    }

    const AudioPipelineConfig config_;
    StereoSpscRing ring_;
    const double nominal_step_;
    std::unique_ptr<float[]> filter_coefficients_;
    double phase_{0.0};
    double integral_correction_{0.0};
    double filtered_occupancy_error_{0.0};
    std::array<StereoFrame, 15> history_{};
    std::size_t history_write_{0};
    std::size_t history_valid_{0};
    std::atomic<std::uint64_t> discontinuity_boundary_{0};
    std::atomic<bool> discontinuity_requested_{false};
    std::atomic<bool> priming_;
    std::atomic<std::uint64_t> input_frames_accepted_{0};
    std::atomic<std::uint64_t> overflow_frames_{0};
    std::atomic<std::uint64_t> dropout_frames_{0};
    std::atomic<std::uint64_t> stale_frames_discarded_{0};
    std::atomic<std::uint64_t> non_finite_samples_sanitized_{0};
    std::atomic<std::uint64_t> discontinuities_{0};
    std::atomic<std::int32_t> correction_ppm_{0};
};

} // namespace axe
