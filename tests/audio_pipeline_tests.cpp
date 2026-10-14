#include "axe/audio_pipeline.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

axe::AudioPipelineConfig small_config() {
    axe::AudioPipelineConfig config;
    config.capacity_frames = 128;
    config.target_fill_frames = 8;
    config.preroll_frames = 32;
    config.max_latency_frames = 96;
    return config;
}

void test_ring_empty_full_wrap_and_stereo() {
    axe::StereoSpscRing ring(4);
    require(ring.available() == 0, "new ring not empty");
    axe::StereoFrame frame{};
    require(!ring.peek(0, frame), "peek succeeded on empty ring");

    const float first[]{10, -10, 20, -20, 30, -30, 40, -40};
    require(ring.push(first, 4) == 4, "ring rejected available capacity");
    require(ring.push(first, 1) == 0, "ring accepted data while full");
    require(ring.available() == 4, "full ring occupancy incorrect");
    require(ring.peek(0, frame) && frame.left == 10 && frame.right == -10,
            "first stereo frame was corrupted");
    ring.consume(2);

    const float second[]{50, -50, 60, -60};
    require(ring.push(second, 2) == 2, "ring failed to write across wrap");
    const float expected_left[]{30, 40, 50, 60};
    const float expected_right[]{-30, -40, -50, -60};
    for (std::size_t i = 0; i < 4; ++i) {
        require(ring.peek(i, frame), "wrapped ring frame missing");
        require(frame.left == expected_left[i] && frame.right == expected_right[i],
                "wrapped ring stereo ordering incorrect");
    }
    ring.consume(4);
    require(ring.available() == 0, "ring did not become empty after consume");
}

void test_ring_spsc_concurrency() {
    constexpr std::uint32_t count = 500'000;
    axe::StereoSpscRing ring(1'024);
    std::atomic<bool> failed{false};

    std::thread producer([&] {
        std::uint32_t next = 1;
        while (next <= count) {
            const std::uint32_t batch = std::min<std::uint32_t>(next % 29 + 1,
                                                                 count - next + 1);
            float values[58]{};
            for (std::uint32_t i = 0; i < batch; ++i) {
                const float value = static_cast<float>(next + i);
                values[i * 2] = value;
                values[i * 2 + 1] = -value;
            }
            const auto accepted = ring.push(values, batch);
            next += static_cast<std::uint32_t>(accepted);
            if (accepted == 0) {
                std::this_thread::yield();
            }
        }
    });

    std::thread consumer([&] {
        std::uint32_t expected = 1;
        while (expected <= count) {
            axe::StereoFrame frame{};
            if (!ring.peek(0, frame)) {
                std::this_thread::yield();
                continue;
            }
            if (frame.left != static_cast<float>(expected) ||
                frame.right != -static_cast<float>(expected)) {
                failed.store(true, std::memory_order_relaxed);
                return;
            }
            ring.consume(1);
            ++expected;
        }
    });

    producer.join();
    consumer.join();
    require(!failed.load(std::memory_order_relaxed), "SPSC data/order corruption detected");
    require(ring.available() == 0, "SPSC stress left unexpected frames");
}

void test_concurrent_stats_occupancy_snapshot() {
    auto config = axe::AudioPipelineConfig{};
    config.capacity_frames = 1'024;
    config.target_fill_frames = 256;
    config.preroll_frames = 64;
    config.max_latency_frames = 900;
    axe::AudioPipeline pipeline(config);
    std::array<float, 256> input{};
    for (std::size_t i = 0; i < input.size() / 2; ++i) {
        input[i * 2] = 0.125f;
        input[i * 2 + 1] = -0.125f;
    }
    std::array<float, 128> left{};
    std::array<float, 128> right{};
    std::atomic<bool> producer_done{false};
    std::atomic<bool> consumer_done{false};
    std::atomic<bool> invalid_snapshot{false};

    std::thread producer([&] {
        for (int i = 0; i < 5'000; ++i) {
            (void)pipeline.ingest(input.data(), 128);
        }
        producer_done.store(true, std::memory_order_release);
    });
    std::thread consumer([&] {
        for (int i = 0; i < 5'000; ++i) {
            pipeline.render(left.data(), right.data(), 64);
        }
        consumer_done.store(true, std::memory_order_release);
    });
    std::thread observer([&] {
        while (!producer_done.load(std::memory_order_acquire) ||
               !consumer_done.load(std::memory_order_acquire)) {
            if (pipeline.stats().buffered_frames > config.capacity_frames) {
                invalid_snapshot.store(true, std::memory_order_relaxed);
                return;
            }
        }
    });
    producer.join();
    consumer.join();
    observer.join();
    require(!invalid_snapshot.load(std::memory_order_relaxed),
            "concurrent stats snapshot escaped ring capacity");
}

void test_pipeline_stereo_and_rate_conversion() {
    auto config = small_config();
    config.capacity_frames = 64;
    config.target_fill_frames = 8;
    config.preroll_frames = 17;
    config.max_latency_frames = 48;
    axe::AudioPipeline pipeline(config);
    float input[40]{};
    for (int i = 0; i < 20; ++i) {
        input[i * 2] = static_cast<float>(i + 1);
        input[i * 2 + 1] = static_cast<float>(-10 * (i + 1));
    }
    require(pipeline.ingest(input, 20) == 20, "pipeline did not ingest complete block");
    float left[4]{};
    float right[4]{};
    pipeline.render(left, right, 4);
    for (int i = 0; i < 4; ++i) {
        require(left[i] == static_cast<float>(i + 1), "left channel changed at unity rate");
        require(right[i] == static_cast<float>(-10 * (i + 1)),
                "right channel crossed or changed at unity rate");
    }

    config.input_rate_hz = 24'000;
    config.output_rate_hz = 48'000;
    config.capacity_frames = 128;
    config.target_fill_frames = 64;
    config.max_latency_frames = 100;
    axe::AudioPipeline upsampler(config);
    float ramp[120]{};
    for (int i = 0; i < 60; ++i) {
        ramp[i * 2] = static_cast<float>(i);
        ramp[i * 2 + 1] = static_cast<float>(-i);
    }
    require(upsampler.ingest(ramp, 60) == 60, "upsampler input rejected");
    float up_left[80]{};
    float up_right[80]{};
    upsampler.render(up_left, up_right, 80);
    for (int i = 30; i < 60; ++i) {
        const float expected = static_cast<float>(i) * 0.5f;
        require(std::fabs(up_left[i] - expected) < 0.15f,
                "sinc resampler lost linear-ramp accuracy");
        require(std::fabs(up_right[i] + expected) < 0.15f,
                "resampling did not preserve stereo channel separation");
    }
}

void test_sanitization_overflow_and_stats() {
    auto config = small_config();
    config.capacity_frames = 32;
    config.target_fill_frames = 4;
    config.preroll_frames = 17;
    config.max_latency_frames = 32;
    axe::AudioPipeline pipeline(config);
    std::array<float, 80> input{};
    input[0] = std::numeric_limits<float>::quiet_NaN();
    input[1] = 1;
    input[2] = 2;
    input[3] = std::numeric_limits<float>::infinity();
    for (std::size_t i = 2; i < input.size() / 2; ++i) {
        input[i * 2] = static_cast<float>(i + 1);
        input[i * 2 + 1] = -static_cast<float>(i + 1);
    }
    require(pipeline.ingest(input.data(), 40) == 32,
            "overflow did not accept exactly capacity");
    const auto stats = pipeline.stats();
    require(stats.overflow_frames == 8, "overflow frame counter incorrect");
    require(stats.input_frames_accepted == 32, "accepted frame counter incorrect");
    require(stats.non_finite_samples_sanitized == 2,
            "non-finite input sample counter incorrect");

    float left[32]{};
    float right[32]{};
    pipeline.render(left, right, 32);
    for (int i = 0; i < 32; ++i) {
        require(std::isfinite(left[i]) && std::isfinite(right[i]),
                "non-finite sample escaped to output");
    }
    require(left[0] == 0.0f && right[0] == 1.0f,
            "non-finite sample was not replaced with channel-local silence");
}

void test_silent_packets_keep_timeline() {
    auto config = small_config();
    config.capacity_frames = 128;
    config.target_fill_frames = 32;
    config.preroll_frames = 17;
    config.max_latency_frames = 100;
    axe::AudioPipeline pipeline(config);
    std::array<float, 40> input{};
    for (std::size_t i = 0; i < 20; ++i) {
        input[i * 2] = static_cast<float>(i + 1);
        input[i * 2 + 1] = -static_cast<float>(i + 1);
    }
    require(pipeline.ingest(input.data(), 20) == 20, "signal packet ingest failed");
    require(pipeline.submit_silence(20) == 20, "silent packet was not accepted");
    std::array<float, 40> left{};
    std::array<float, 40> right{};
    pipeline.render(left.data(), right.data(), 40);
    for (std::size_t i = 0; i < 20; ++i) {
        require(left[i] == static_cast<float>(i + 1) &&
                    right[i] == -static_cast<float>(i + 1),
                "silent packet changed preceding stereo samples");
        require(left[i + 20] == 0 && right[i + 20] == 0,
                "silent packet did not produce zero-valued frames");
    }
    require(pipeline.stats().input_frames_accepted == 40,
            "silent packet frames were omitted from input accounting");
}

void test_preroll_underrun_and_fresh_recovery() {
    auto config = small_config();
    config.preroll_frames = 17;
    axe::AudioPipeline pipeline(config);
    float short_input[16]{};
    for (int i = 0; i < 8; ++i) {
        short_input[i * 2] = static_cast<float>(i + 1);
        short_input[i * 2 + 1] = static_cast<float>(-i - 1);
    }
    require(pipeline.ingest(short_input, 8) == 8, "short input ingest failed");
    float left[4]{9, 9, 9, 9};
    float right[4]{9, 9, 9, 9};
    pipeline.render(left, right, 4);
    for (int i = 0; i < 4; ++i) {
        require(left[i] == 0 && right[i] == 0, "preroll did not render silence");
    }
    require(pipeline.stats().priming, "pipeline left priming before enough data arrived");

    pipeline.notify_discontinuity();
    pipeline.render(left, right, 4);
    std::array<float, 40> fresh{};
    for (int i = 0; i < 20; ++i) {
        fresh[i * 2] = static_cast<float>(100 * (i + 1));
        fresh[i * 2 + 1] = static_cast<float>(-100 * (i + 1));
    }
    require(pipeline.ingest(fresh.data(), 20) == 20, "fresh input ingest failed");
    pipeline.render(left, right, 3);
    require(left[0] == 100 && right[0] == -100 && left[2] == 300 && right[2] == -300,
            "pipeline failed to resume with fresh stereo data");
    require(pipeline.stats().dropout_frames == 8, "dropout frame count incorrect");
}

void test_discontinuity_flush_and_stale_recovery() {
    auto config = small_config();
    config.capacity_frames = 1'024;
    config.target_fill_frames = 100;
    config.preroll_frames = 17;
    config.max_latency_frames = 300;
    axe::AudioPipeline pipeline(config);
    std::array<float, 2'000> input{};
    for (std::size_t i = 0; i < 1'000; ++i) {
        input[i * 2] = static_cast<float>(i);
        input[i * 2 + 1] = -static_cast<float>(i);
    }
    require(pipeline.ingest(input.data(), 1'000) == 1'000, "stale test ingest failed");
    float left[8]{};
    float right[8]{};
    pipeline.render(left, right, 8);
    require(left[0] >= 900.0f, "stale recovery played old queued audio");
    auto stats = pipeline.stats();
    require(stats.stale_frames_discarded == 900,
            "stale frame count did not return queue to target fill");
    require(stats.discontinuities >= 1, "stale recovery did not record a discontinuity");

    pipeline.notify_discontinuity();
    pipeline.render(left, right, 4);
    for (int i = 0; i < 4; ++i) {
        require(left[i] == 0 && right[i] == 0, "explicit discontinuity did not flush queue");
    }
    float fresh[40]{};
    for (int i = 0; i < 20; ++i) {
        fresh[i * 2] = static_cast<float>(7 + i);
        fresh[i * 2 + 1] = static_cast<float>(-7 - i);
    }
    require(pipeline.ingest(fresh, 20) == 20, "fresh post-discontinuity ingest failed");
    pipeline.render(left, right, 4);
    require(left[0] == 7 && right[0] == -7 && left[3] == 10 && right[3] == -10,
            "explicit discontinuity did not resume after a fresh preroll");
    stats = pipeline.stats();
    require(stats.discontinuities >= 2, "explicit discontinuity was not counted");
}

void test_discontinuity_preserves_post_boundary_packet() {
    auto config = axe::AudioPipelineConfig{};
    config.capacity_frames = 256;
    config.target_fill_frames = 64;
    config.preroll_frames = 32;
    config.max_latency_frames = 192;
    axe::AudioPipeline pipeline(config);
    std::array<float, 256> input{};
    for (std::size_t i = 0; i < 64; ++i) {
        input[i * 2] = 1.0f;
        input[i * 2 + 1] = -1.0f;
    }
    require(pipeline.ingest(input.data(), 64) == 64, "boundary test warmup ingest failed");
    std::array<float, 32> left{};
    std::array<float, 32> right{};
    pipeline.render(left.data(), right.data(), 32);
    require(!pipeline.stats().priming, "boundary test failed to enter streaming state");

    for (std::size_t i = 0; i < 64; ++i) {
        input[i * 2] = 0.5f;
        input[i * 2 + 1] = -0.25f;
    }
    pipeline.notify_discontinuity();
    require(pipeline.ingest(input.data(), 64) == 64, "post-boundary packet ingest failed");
    pipeline.render(left.data(), right.data(), 32);
    require(left[0] == 0.5f && right[0] == -0.25f,
            "discontinuity flush discarded the packet submitted after its boundary");
    require(!pipeline.stats().priming,
            "already-running stream unnecessarily waited for a full preroll after a gap");
}

void run_drift_case(double source_clock_error, bool expect_positive_correction) {
    auto config = axe::AudioPipelineConfig{};
    config.capacity_frames = 8'192;
    config.target_fill_frames = 1'024;
    config.preroll_frames = 256;
    config.max_latency_frames = 8'000;
    axe::AudioPipeline pipeline(config);

    std::array<float, 512> samples{};
    for (std::size_t i = 0; i < samples.size() / 2; ++i) {
        samples[i * 2] = 0.25f;
        samples[i * 2 + 1] = -0.25f;
    }
    // Establish the target queue before the drift and jitter simulation.
    for (int i = 0; i < 4; ++i) {
        require(pipeline.ingest(samples.data(), 256) == 256,
                "drift warmup ingest failed");
    }
    float output_left[256]{};
    float output_right[256]{};
    pipeline.render(output_left, output_right, 256);

    double fractional_frames = 0.0;
    constexpr int blocks = 10'000;
    for (int block = 0; block < blocks; ++block) {
        fractional_frames += 256.0 * source_clock_error;
        std::uint32_t input_frames = 256;
        if (fractional_frames >= 1.0) {
            ++input_frames;
            fractional_frames -= 1.0;
        } else if (fractional_frames <= -1.0) {
            --input_frames;
            fractional_frames += 1.0;
        }

        // Deterministic packet jitter with zero long-term mean.
        switch (block & 3) {
        case 1:
            ++input_frames;
            break;
        case 3:
            --input_frames;
            break;
        default:
            break;
        }
        require(pipeline.ingest(samples.data(), input_frames) == input_frames,
                "drift simulation unexpectedly overflowed");
        pipeline.render(output_left, output_right, 256);
    }

    const auto stats = pipeline.stats();
    require(stats.dropout_frames == 0, "clock drift simulation had underruns");
    require(stats.buffered_frames > 256 && stats.buffered_frames < 2'000,
            "clock drift queue exceeded its latency bounds");
    require(expect_positive_correction ? stats.correction_ppm > 0
                                       : stats.correction_ppm < 0,
            "adaptive correction did not follow the source clock drift direction");
    require(std::abs(stats.correction_ppm) < config.max_correction_ppm,
            "adaptive correction saturated during ordinary clock drift");
    for (int i = 0; i < 256; ++i) {
        require(std::isfinite(output_left[i]) && std::isfinite(output_right[i]),
                "clock correction produced a non-finite sample");
    }
}

void test_clock_drift_both_signs_with_packet_jitter() {
    run_drift_case(+200.0e-6, true);
    run_drift_case(-200.0e-6, false);
}

void run_hour_packet_clock_case(double source_clock_error, bool expect_positive_correction) {
    constexpr std::uint32_t rate = 48'000;
    constexpr std::uint32_t asio_block = 128;
    constexpr std::uint32_t wasapi_packet = 480;
    constexpr std::uint64_t output_frames = 3'600ull * rate;
    constexpr std::uint64_t blocks = output_frames / asio_block;
    constexpr std::uint64_t outage_start = output_frames / 2;
    constexpr std::uint64_t outage_length = rate / 10;
    constexpr std::uint64_t outage_end = outage_start + outage_length;

    auto config = axe::AudioPipelineConfig{};
    config.capacity_frames = 8'192;
    config.target_fill_frames = 960;
    config.preroll_frames = 960;
    config.max_latency_frames = 1'920;
    axe::AudioPipeline pipeline(config);

    std::array<float, wasapi_packet * 2> packet{};
    for (std::size_t i = 0; i < wasapi_packet; ++i) {
        packet[i * 2] = 0.2f;
        packet[i * 2 + 1] = -0.2f;
    }
    require(pipeline.ingest(packet.data(), wasapi_packet) == wasapi_packet,
            "packet-clock initial packet ingest failed");
    std::array<float, asio_block> left{};
    std::array<float, asio_block> right{};

    constexpr double packet_jitter_frames[]{24.0, -24.0, 16.0, -16.0};
    const double packet_period = static_cast<double>(wasapi_packet) /
                                 (1.0 + source_clock_error);
    double next_packet_frame = packet_period + packet_jitter_frames[0];
    std::uint64_t output_time = 0;
    std::uint64_t packet_index = 0;
    bool outage_restarted = false;
    for (std::uint64_t block = 0; block < blocks; ++block) {
        output_time += asio_block;
        if (output_time >= outage_end && !outage_restarted) {
            // Model loopback capture missing 100 ms, then a marked first packet
            // after recovery. `notify_discontinuity` runs before that packet.
            pipeline.notify_discontinuity();
            next_packet_frame = static_cast<double>(output_time) + packet_period +
                                packet_jitter_frames[packet_index & 3];
            outage_restarted = true;
        }

        const bool in_outage = output_time > outage_start && output_time <= outage_end;
        if (!in_outage && (outage_restarted || output_time < outage_start)) {
            while (next_packet_frame <= static_cast<double>(output_time)) {
                require(pipeline.ingest(packet.data(), wasapi_packet) == wasapi_packet,
                        "packet-clock input overflowed during a one-hour drift run");
                ++packet_index;
                next_packet_frame += packet_period + packet_jitter_frames[packet_index & 3];
            }
        }
        pipeline.render(left.data(), right.data(), asio_block);
    }

    const auto stats = pipeline.stats();
    require(outage_restarted, "one-hour packet simulation did not execute recovery");
    require(stats.overflow_frames == 0, "packet clock overflowed during long drift run");
    require(stats.dropout_frames < 16'000,
            "packet clock suffered sustained silence outside startup and the outage");
    require(stats.stale_frames_discarded < 64,
            "packet boundaries discarded more than residual pre-gap audio");
    require(stats.buffered_frames > 128 && stats.buffered_frames < 2'000,
            "packet clock queue escaped latency bounds after recovery");
    require(expect_positive_correction ? stats.correction_ppm > 0
                                       : stats.correction_ppm < 0,
            "hour-long packet clock correction has the wrong sign");
    require(std::abs(stats.correction_ppm) < config.max_correction_ppm,
            "hour-long packet clock correction saturated");
    require(!stats.priming, "hour-long packet stream did not recover from the outage");
}

void test_hour_wasapi_packets_and_outage_both_clock_signs() {
    run_hour_packet_clock_case(+200.0e-6, true);
    run_hour_packet_clock_case(-200.0e-6, false);
}

double measure_tone_amplitude(std::uint32_t tone_hz, std::uint32_t input_rate,
                              std::uint32_t output_rate, std::uint32_t measured_hz) {
    auto config = axe::AudioPipelineConfig{};
    config.input_rate_hz = input_rate;
    config.output_rate_hz = output_rate;
    config.capacity_frames = 16'384;
    config.target_fill_frames = 4'096;
    config.preroll_frames = 128;
    config.max_latency_frames = 16'384;
    axe::AudioPipeline pipeline(config);

    constexpr std::uint32_t input_frames = 8'192;
    std::array<float, input_frames * 2> input{};
    constexpr double pi = 3.1415926535897932384626433832795;
    for (std::uint32_t i = 0; i < input_frames; ++i) {
        const double angle = 2.0 * pi * tone_hz * i / input_rate;
        const float sample = static_cast<float>(std::sin(angle));
        input[i * 2] = sample;
        input[i * 2 + 1] = -sample;
    }
    require(pipeline.ingest(input.data(), input_frames) == input_frames,
            "passband input ingest failed");
    constexpr std::uint32_t output_frames = 2'048;
    std::array<float, output_frames> left{};
    std::array<float, output_frames> right{};
    pipeline.render(left.data(), right.data(), output_frames);

    double sine = 0.0;
    double cosine = 0.0;
    constexpr std::uint32_t first = 128;
    constexpr std::uint32_t last = output_frames;
    for (std::uint32_t i = first; i < last; ++i) {
        const double angle = 2.0 * pi * measured_hz * i / output_rate;
        sine += static_cast<double>(left[i]) * std::sin(angle);
        cosine += static_cast<double>(left[i]) * std::cos(angle);
        require(std::fabs(left[i] + right[i]) < 1.0e-5f,
                "filter changed left/right polarity relationship");
    }
    const double count = static_cast<double>(last - first);
    return 2.0 * std::hypot(sine, cosine) / count;
}

void test_sinc_resampler_passband() {
    for (const std::uint32_t tone : {1'000u, 10'000u, 20'000u}) {
        const double amplitude = measure_tone_amplitude(tone, 48'001, 48'000, tone);
        std::cout << "[INFO] sinc passband " << tone << " Hz amplitude=" << amplitude << '\n';
        const double minimum = tone == 20'000 ? 0.96 : 0.99;
        require(amplitude >= minimum && amplitude <= 1.01,
                "sinc resampler passband response outside tolerance");
    }
    const double alias_amplitude = measure_tone_amplitude(30'000, 96'000, 48'000, 18'000);
    std::cout << "[INFO] sinc downsample alias at 18 kHz amplitude=" << alias_amplitude << '\n';
    require(alias_amplitude < 0.02, "downsampler failed to suppress an out-of-band tone");
}

void test_render_callback_cost() {
    constexpr std::uint32_t block_frames = 256;
    auto config = axe::AudioPipelineConfig{};
    config.input_rate_hz = 48'001;
    config.output_rate_hz = 48'000;
    config.capacity_frames = 8'192;
    config.target_fill_frames = 1'024;
    config.preroll_frames = 128;
    config.max_latency_frames = 8'000;
    axe::AudioPipeline pipeline(config);
    std::array<float, block_frames * 2> input{};
    std::array<float, block_frames> left{};
    std::array<float, block_frames> right{};
    for (std::size_t i = 0; i < block_frames; ++i) {
        input[i * 2] = 0.125f;
        input[i * 2 + 1] = -0.125f;
    }
    for (int i = 0; i < 4; ++i) {
        (void)pipeline.ingest(input.data(), block_frames);
    }
    pipeline.render(left.data(), right.data(), block_frames);

    constexpr int measured_blocks = 10'000;
    const auto start = std::chrono::steady_clock::now();
    std::array<double, measured_blocks> per_block_us{};
    for (int i = 0; i < measured_blocks; ++i) {
        const auto block_start = std::chrono::steady_clock::now();
        (void)pipeline.ingest(input.data(), block_frames);
        pipeline.render(left.data(), right.data(), block_frames);
        per_block_us[i] =
            std::chrono::duration<double, std::micro>(
                std::chrono::steady_clock::now() - block_start)
                .count();
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;
    const double elapsed_us =
        std::chrono::duration<double, std::micro>(elapsed).count();
    const double average_us = elapsed_us / measured_blocks;
    const double deadline_us = 1'000'000.0 * block_frames / config.output_rate_hz;
    std::sort(per_block_us.begin(), per_block_us.end());
    const double p99_us = per_block_us[measured_blocks * 99 / 100];
    const double max_us = per_block_us.back();
    std::cout << "[INFO] sinc render+ingest average=" << average_us
              << " us, p99=" << p99_us << " us, max=" << max_us
              << " us, block deadline=" << deadline_us << " us\n";
    require(average_us < deadline_us,
            "average callback processing time exceeded the audio block deadline");
}

void test_config_validation() {
    auto config = small_config();
    config.output_rate_hz = 0;
    bool threw = false;
    try {
        axe::AudioPipeline pipeline(config);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    require(threw, "invalid sample rate was not rejected at construction");
}

} // namespace

int main() {
    const std::array tests{
        std::pair{"ring empty/full/wrap/stereo", test_ring_empty_full_wrap_and_stereo},
        std::pair{"ring SPSC concurrency", test_ring_spsc_concurrency},
        std::pair{"concurrent stats occupancy snapshot", test_concurrent_stats_occupancy_snapshot},
        std::pair{"pipeline stereo and sample-rate conversion", test_pipeline_stereo_and_rate_conversion},
        std::pair{"sanitization, overflow, and counters", test_sanitization_overflow_and_stats},
        std::pair{"silent packets preserve the source timeline", test_silent_packets_keep_timeline},
        std::pair{"preroll, underrun, and recovery", test_preroll_underrun_and_fresh_recovery},
        std::pair{"discontinuity flush and stale recovery", test_discontinuity_flush_and_stale_recovery},
        std::pair{"discontinuity preserves the post-boundary packet", test_discontinuity_preserves_post_boundary_packet},
        std::pair{"clock drift both signs with jitter", test_clock_drift_both_signs_with_packet_jitter},
        std::pair{"one-hour WASAPI packets and outage, both clock signs", test_hour_wasapi_packets_and_outage_both_clock_signs},
        std::pair{"sinc resampler passband", test_sinc_resampler_passband},
        std::pair{"render callback cost", test_render_callback_cost},
        std::pair{"configuration validation", test_config_validation},
    };

    try {
        for (const auto& [name, test] : tests) {
            test();
            std::cout << "[PASS] " << name << '\n';
        }
    } catch (const std::exception& error) {
        std::cerr << "[FAIL] " << error.what() << '\n';
        return 1;
    }
    return 0;
}
