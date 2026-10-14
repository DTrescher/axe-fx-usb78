#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace axe::diagnostic {

// Diagnostic-only Goertzel banks. Each one-second window is measured
// independently so small clock corrections do not accumulate phase error.
struct SignalMetrics {
    static constexpr std::uint32_t frames_per_window = 48'000;

    const std::array<double, 2> coefficients{
        2.0 * std::cos(6.283185307179586 * 997.0 / frames_per_window),
        2.0 * std::cos(6.283185307179586 * 1511.0 / frames_per_window),
    };
    std::array<double, 4> previous{};
    std::array<double, 4> before_previous{};
    std::array<double, 4> accumulated_power{};
    std::uint32_t frames_in_window{};
    std::uint64_t completed_windows{};

    void sample(float left, float right) noexcept {
        for (std::size_t bank = 0; bank < previous.size(); ++bank) {
            const double input = bank < 2 ? left : right;
            const double value = input + coefficients[bank % 2] * previous[bank] -
                                 before_previous[bank];
            before_previous[bank] = previous[bank];
            previous[bank] = value;
        }

        if (++frames_in_window != frames_per_window) {
            return;
        }

        const double normalization =
            2.0 / (static_cast<double>(frames_per_window) * frames_per_window);
        for (std::size_t bank = 0; bank < previous.size(); ++bank) {
            const double power =
                previous[bank] * previous[bank] +
                before_previous[bank] * before_previous[bank] -
                coefficients[bank % 2] * previous[bank] * before_previous[bank];
            accumulated_power[bank] += power * normalization;
            previous[bank] = 0.0;
            before_previous[bank] = 0.0;
        }
        frames_in_window = 0;
        ++completed_windows;
    }

    [[nodiscard]] double rms(std::size_t channel, std::size_t tone) const noexcept {
        if (completed_windows == 0 || channel >= 2 || tone >= 2) {
            return 0.0;
        }
        const std::size_t bank = channel * 2 + tone;
        const double mean_power = accumulated_power[bank] / completed_windows;
        return std::sqrt(std::max(0.0, mean_power));
    }

    [[nodiscard]] bool matches_stereo_tones(float left_gain = 1.0f,
                                            float right_gain = 1.0f) const noexcept {
        if (left_gain <= 0.0f || right_gain <= 0.0f || completed_windows == 0) {
            return false;
        }

        const double left_tone = rms(0, 0);
        const double right_tone = rms(1, 1);
        const double normalized_ratio =
            (left_tone / left_gain) / (right_tone / right_gain);

        return left_tone > 0.0001 * left_gain &&
               right_tone > 0.00005 * right_gain &&
               left_tone > 10.0 * rms(0, 1) &&
               right_tone > 10.0 * rms(1, 0) &&
               normalized_ratio > 1.8 && normalized_ratio < 2.2;
    }
};

}  // namespace axe::diagnostic
