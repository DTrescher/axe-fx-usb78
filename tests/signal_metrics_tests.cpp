#include "signal_metrics.hpp"

#include <cmath>
#include <cstdint>
#include <exception>
#include <initializer_list>
#include <iostream>
#include <stdexcept>

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr std::uint32_t kTestFrames = 144'000;

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void sample_stereo_tones(axe::diagnostic::SignalMetrics& metrics,
                         double drift,
                         float left_gain = 1.0f,
                         float right_gain = 1.0f) {
    for (std::uint32_t frame = 0; frame < kTestFrames; ++frame) {
        const double time = static_cast<double>(frame) * (1.0 + drift) / 48'000.0;
        const float left = static_cast<float>(
            left_gain * 0.0005 * std::sin(2.0 * kPi * 997.0 * time));
        const float right = static_cast<float>(
            right_gain * 0.00025 * std::sin(2.0 * kPi * 1511.0 * time));
        metrics.sample(left, right);
    }
}

void test_tone_identity_and_clock_drift() {
    for (const double drift : {0.0, 0.0002, -0.0002}) {
        axe::diagnostic::SignalMetrics metrics;
        sample_stereo_tones(metrics, drift);
        require(metrics.matches_stereo_tones(),
                "Expected stereo tones were rejected");
    }
}

void test_endpoint_gain_calibration() {
    axe::diagnostic::SignalMetrics metrics;
    sample_stereo_tones(metrics, 0.0, 0.25f, 0.5f);
    require(metrics.matches_stereo_tones(0.25f, 0.5f),
            "Endpoint gain and balance calibration did not match");
}

void test_unrelated_audio_is_rejected() {
    axe::diagnostic::SignalMetrics swapped;
    axe::diagnostic::SignalMetrics dc;
    axe::diagnostic::SignalMetrics noise;
    std::uint32_t random = 12'345;

    for (std::uint32_t frame = 0; frame < kTestFrames; ++frame) {
        const double time = static_cast<double>(frame) / 48'000.0;
        swapped.sample(
            static_cast<float>(0.0005 * std::sin(2.0 * kPi * 1511.0 * time)),
            static_cast<float>(0.00025 * std::sin(2.0 * kPi * 997.0 * time)));
        dc.sample(0.000353553f, 0.000176777f);

        random = random * 1'664'525u + 1'013'904'223u;
        const float value =
            (static_cast<float>(random) / 4'294'967'295.0f - 0.5f) * 0.001225f;
        noise.sample(value, value * 0.5f);
    }

    require(!swapped.matches_stereo_tones() &&
                !dc.matches_stereo_tones() &&
                !noise.matches_stereo_tones(),
            "Unrelated audio incorrectly passed signal verification");
}

}  // namespace

int main() {
    try {
        test_tone_identity_and_clock_drift();
        test_endpoint_gain_calibration();
        test_unrelated_audio_is_rejected();
        std::cout << "PASS: diagnostic tone identity, stereo isolation, drift "
                     "tolerance, and unrelated-audio rejection\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
