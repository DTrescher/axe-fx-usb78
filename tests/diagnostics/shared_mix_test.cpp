#include "windows_audio.hpp"
#include <endpointvolume.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr double kRate = 48000.0;
constexpr double kRateTolerance = 0.01;
constexpr double kLeftFrequency = 997.0;
constexpr double kRightFrequency = 1511.0;
constexpr float kLeftPeak = 0.0005f;  // -66 dBFS, intentionally quiet.
constexpr float kRightPeak = 0.00025f; // -72 dBFS, intentionally quiet.

struct CaptureMetrics {
    std::atomic<bool> measuring{false};
    uint64_t frames{};
    uint64_t silent_frames{};
    uint64_t discontinuities{};
    uint64_t non_finite_samples{};
    std::array<std::array<double, 2>, 2> in_phase{};
    std::array<std::array<double, 2>, 2> quadrature{};
    double cosine[2]{1.0, 1.0};
    double sine[2]{};
    double step_cosine[2]{std::cos(2.0 * kPi * kLeftFrequency / kRate),
                          std::cos(2.0 * kPi * kRightFrequency / kRate)};
    double step_sine[2]{std::sin(2.0 * kPi * kLeftFrequency / kRate),
                        std::sin(2.0 * kPi * kRightFrequency / kRate)};
};

struct CaptureContext {
    CaptureMetrics* metrics{};
    HANDLE ready{};
};

void capture_ready(void* opaque) {
    auto& context = *static_cast<CaptureContext*>(opaque);
    SetEvent(context.ready);
}

void capture_sink(void* opaque, const float* interleaved, uint32_t frames, bool discontinuity) {
    auto& context = *static_cast<CaptureContext*>(opaque);
    auto& metrics = *context.metrics;
    if (!metrics.measuring.load(std::memory_order_relaxed)) return;

    if (discontinuity) ++metrics.discontinuities;
    if (!interleaved) metrics.silent_frames += frames;
    for (uint32_t frame = 0; frame < frames; ++frame) {
        float channels[2]{};
        if (interleaved) {
            channels[0] = interleaved[2u * frame];
            channels[1] = interleaved[2u * frame + 1u];
        }
        ++metrics.frames;
        for (unsigned channel = 0; channel < 2; ++channel) {
            if (!std::isfinite(channels[channel])) {
                ++metrics.non_finite_samples;
                channels[channel] = 0.0f;
            }
            for (unsigned tone = 0; tone < 2; ++tone) {
                metrics.in_phase[channel][tone] += channels[channel] * metrics.cosine[tone];
                metrics.quadrature[channel][tone] += channels[channel] * metrics.sine[tone];
            }
        }
        for (unsigned tone = 0; tone < 2; ++tone) {
            const double next_cosine = metrics.cosine[tone] * metrics.step_cosine[tone] -
                                       metrics.sine[tone] * metrics.step_sine[tone];
            const double next_sine = metrics.sine[tone] * metrics.step_cosine[tone] +
                                     metrics.cosine[tone] * metrics.step_sine[tone];
            metrics.cosine[tone] = next_cosine;
            metrics.sine[tone] = next_sine;
        }
    }
}

double tone_peak(const CaptureMetrics& metrics, unsigned channel, unsigned tone) {
    if (!metrics.frames) return 0.0;
    const double i = metrics.in_phase[channel][tone];
    const double q = metrics.quadrature[channel][tone];
    return 2.0 * std::sqrt(i * i + q * q) / static_cast<double>(metrics.frames);
}

struct StopEvent {
    HANDLE value{};
    ~StopEvent() { if (value) CloseHandle(value); }
};

bool accepted_name(const std::wstring& name) {
    return name == L"Speakers (Axe-Fx III)";
}

struct Worker {
    std::string error;
    axe::win::RenderTestTiming timing;
};

// Hardware validation establishes that native FAS loopback includes the
// endpoint's absolute channel levels. This test checks session mixing against
// those levels without applying them again to the captured samples.
std::array<double, 2> captured_endpoint_levels(const std::wstring& id) {
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    axe::win::check(initialized, "Shared-mix volume COM");
    struct ComEnd { ~ComEnd() { CoUninitialize(); } } com_end;
    Microsoft::WRL::ComPtr<IMMDeviceEnumerator> enumerator;
    axe::win::check(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                     IID_PPV_ARGS(&enumerator)), "Shared-mix enumerator");
    Microsoft::WRL::ComPtr<IMMDevice> device;
    axe::win::check(enumerator->GetDevice(id.c_str(), &device), "Shared-mix volume device");
    Microsoft::WRL::ComPtr<IAudioEndpointVolume> volume;
    axe::win::check(device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
                                     &volume), "Shared-mix endpoint volume");
    BOOL mute{};
    axe::win::check(volume->GetMute(&mute), "Shared-mix mute");
    if (mute) throw std::runtime_error("Unmute the native endpoint before testing session mixing");
    std::array<double, 2> levels{};
    for (UINT channel = 0; channel < 2; ++channel) {
        float db{};
        axe::win::check(volume->GetChannelVolumeLevel(channel, &db), "Shared-mix channel level");
        levels[channel] = std::pow(10.0, static_cast<double>(db) / 20.0);
        if (!std::isfinite(levels[channel]) || levels[channel] <= 0)
            throw std::runtime_error("Native endpoint volume is invalid or silent");
    }
    return levels;
}

void validate_period(const Worker& worker, const char* label) {
    const auto& t = worker.timing;
    std::cout << label << " audio_client3=" << (t.used_audio_client3 ? "yes" : "no")
              << " advertised_default=" << t.default_engine_frames
              << " fundamental=" << t.fundamental_frames
              << " minimum=" << t.minimum_engine_frames
              << " maximum=" << t.maximum_engine_frames
              << " initialized=" << t.initialized_engine_frames
              << " current=" << t.current_engine_frames
              << " buffer=" << t.buffer_frames
              << " event_wakes=" << t.event_wake_count
              << " interval_ms(mean/min/max)=" << t.mean_event_interval_ms << '/'
              << t.minimum_event_interval_ms << '/' << t.maximum_event_interval_ms << '\n';
    if (t.used_audio_client3) {
        const bool valid_limits = t.minimum_engine_frames != 0 &&
                                  t.fundamental_frames != 0 &&
                                  t.maximum_engine_frames >= t.minimum_engine_frames;
        const bool initialized_in_range = valid_limits &&
            t.initialized_engine_frames >= t.minimum_engine_frames &&
            t.initialized_engine_frames <= t.maximum_engine_frames;
        const bool initialized_is_fundamental_multiple = valid_limits &&
            t.initialized_engine_frames % t.fundamental_frames == 0;
        if (!initialized_in_range || !initialized_is_fundamental_multiple ||
            t.current_engine_frames != t.initialized_engine_frames) {
            throw std::runtime_error(std::string(label) +
                " actual IAudioClient3 period was outside the advertised range, was not a fundamental-period multiple, or differed from the initialized period");
        }
    }
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        std::cerr << "Usage: axe-shared-mix-test <explicit native FAS endpoint ID> [--seconds N] [--verify-session-volume]\n";
        return 2;
    }

    unsigned seconds = 10;
    bool verify_session_volume = false;
    for (int index = 2; index < argc; ++index) {
        const std::wstring argument = argv[index];
        if (argument == L"--verify-session-volume") {
            if (verify_session_volume) {
                std::cerr << "--verify-session-volume may be specified only once.\n";
                return 2;
            }
            verify_session_volume = true;
            continue;
        }
        if (argument != L"--seconds" || index + 1 >= argc) {
            std::cerr << "Usage: axe-shared-mix-test <explicit native FAS endpoint ID> [--seconds N] [--verify-session-volume]\n";
            return 2;
        }
        try {
            const std::wstring value = argv[++index];
            size_t consumed{};
            const auto parsed = std::stoul(value, &consumed);
            if (consumed != value.size() || parsed < 5 || parsed > 60)
                throw std::invalid_argument("Invalid duration");
            seconds = static_cast<unsigned>(parsed);
        } catch (...) {
            std::cerr << "Duration must be an integer from 5 to 60 seconds.\n";
            return 2;
        }
    }
    if (seconds < 5 || seconds > 60) {
        std::cerr << "Duration must be an integer from 5 to 60 seconds.\n";
        return 2;
    }
    const float left_session_gain = verify_session_volume ? 0.5f : 1.0f;
    const float right_session_gain = verify_session_volume ? 0.25f : 1.0f;

    StopEvent stop{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    StopEvent capture_is_ready{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    if (!stop.value || !capture_is_ready.value) {
        std::cerr << "Could not create test events.\n";
        return 1;
    }

    std::thread capture_thread;
    std::thread left_thread;
    std::thread right_thread;
    CaptureMetrics metrics;
    CaptureContext capture_context{&metrics, capture_is_ready.value};
    Worker left_worker, right_worker;
    uint32_t capture_period{}, capture_buffer_frames{};
    HRESULT capture_low_latency_result{E_NOINTERFACE};
    std::string capture_error;

    try {
        const std::wstring endpoint_id = argv[1];
        const auto endpoints = axe::win::enumerate_render();
        const auto selected = std::find_if(endpoints.begin(), endpoints.end(), [&](const auto& endpoint) {
            return endpoint.id == endpoint_id;
        });
        if (selected == endpoints.end()) throw std::runtime_error("The explicitly selected endpoint is not active");
        if (!accepted_name(selected->name))
            throw std::runtime_error("Select the native FAS Speakers (Axe-Fx III) endpoint by its explicit ID");
        if (selected->rate != 48000 || selected->channels != 2 || !selected->ieee_float)
            throw std::runtime_error("Selected endpoint must expose stereo 48kHz float shared audio");
        const auto native_levels = captured_endpoint_levels(endpoint_id);

        std::cout << "Endpoint: " << axe::win::utf8(selected->name)
                  << " (explicit ID; default endpoint is not changed)\n"
                  << "Test signals: left-only 997Hz -66dBFS; right-only 1511Hz -72dBFS; duration="
                  << seconds << "s; endpoint volume is not changed; new session gains L/R="
                  << left_session_gain << '/' << right_session_gain << ".\n";

        capture_thread = std::thread([&] {
            try {
                axe::win::capture_loopback(endpoint_id, stop.value, capture_sink, &capture_context,
                                           capture_period, capture_ready, &capture_low_latency_result,
                                           &capture_buffer_frames);
            } catch (const std::exception& e) {
                capture_error = e.what();
                SetEvent(stop.value);
            }
        });

        HANDLE startup_waits[]{capture_is_ready.value, stop.value};
        const DWORD startup_wait = WaitForMultipleObjects(2, startup_waits, FALSE, 10000);
        if (startup_wait != WAIT_OBJECT_0)
            throw std::runtime_error(startup_wait == WAIT_OBJECT_0 + 1
                ? "WASAPI loopback capture failed during startup"
                : "WASAPI loopback capture did not become ready within 10 seconds");

        left_thread = std::thread([&] {
            try { axe::win::render_test_signal(endpoint_id, stop.value, kLeftPeak, 0.0f,
                                               &left_worker.timing, 0, left_session_gain); }
            catch (const std::exception& e) { left_worker.error = e.what(); SetEvent(stop.value); }
        });
        right_thread = std::thread([&] {
            try { axe::win::render_test_signal(endpoint_id, stop.value, 0.0f, kRightPeak,
                                               &right_worker.timing, 0, right_session_gain); }
            catch (const std::exception& e) { right_worker.error = e.what(); SetEvent(stop.value); }
        });

        if (WaitForSingleObject(stop.value, 1000) == WAIT_OBJECT_0)
            throw std::runtime_error("A WASAPI client failed during startup");
        metrics.measuring.store(true, std::memory_order_relaxed);
        const auto measurement_start = std::chrono::steady_clock::now();
        const DWORD measurement_wait = WaitForSingleObject(stop.value, seconds * 1000);
        const auto measurement_end = std::chrono::steady_clock::now();
        metrics.measuring.store(false, std::memory_order_relaxed);
        SetEvent(stop.value);

        if (left_thread.joinable()) left_thread.join();
        if (right_thread.joinable()) right_thread.join();
        if (capture_thread.joinable()) capture_thread.join();
        if (!capture_error.empty()) throw std::runtime_error(capture_error);
        if (!left_worker.error.empty()) throw std::runtime_error(std::string("Left client: ") + left_worker.error);
        if (!right_worker.error.empty()) throw std::runtime_error(std::string("Right client: ") + right_worker.error);
        if (measurement_wait != WAIT_TIMEOUT)
            throw std::runtime_error("A WASAPI client stopped before the measurement interval completed");

        std::cout << "capture_engine_period=" << capture_period
                  << " capture_buffer=" << capture_buffer_frames
                  << " capture_audio_client3_init_hr=0x" << std::hex
                  << static_cast<uint32_t>(capture_low_latency_result) << std::dec << '\n';
        validate_period(left_worker, "left_client");
        validate_period(right_worker, "right_client");

        const double left_997 = tone_peak(metrics, 0, 0);
        const double left_1511 = tone_peak(metrics, 0, 1);
        const double right_997 = tone_peak(metrics, 1, 0);
        const double right_1511 = tone_peak(metrics, 1, 1);
        const double measured_seconds = std::chrono::duration<double>(
            measurement_end - measurement_start).count();
        const double capture_rate = measured_seconds > 0.0
            ? static_cast<double>(metrics.frames) / measured_seconds : 0.0;
        const bool capture_rate_ok = std::isfinite(capture_rate) &&
            capture_rate >= kRate * (1.0 - kRateTolerance) &&
            capture_rate <= kRate * (1.0 + kRateTolerance);
        std::cout << std::fixed << std::setprecision(8)
                  << "measured_frames=" << metrics.frames
                  << " measured_seconds=" << std::setprecision(3) << measured_seconds
                  << " capture_rate_hz=" << std::setprecision(2) << capture_rate
                  << " silent_frames=" << metrics.silent_frames
                  << " discontinuities=" << metrics.discontinuities
                  << " non_finite=" << metrics.non_finite_samples << '\n'
                  << std::scientific << std::setprecision(6)
                  << "loopback_tone_peak left(997/1511)=" << left_997 << '/' << left_1511
                  << " right(997/1511)=" << right_997 << '/' << right_1511 << '\n';

        if (captured_endpoint_levels(endpoint_id) != native_levels)
            throw std::runtime_error("Endpoint levels changed during the shared-mix diagnostic");
        const double expected_left_peak = static_cast<double>(kLeftPeak) * left_session_gain * native_levels[0];
        const double expected_right_peak = static_cast<double>(kRightPeak) * right_session_gain * native_levels[1];
        const bool levels_ok = left_997 >= expected_left_peak * 0.65 &&
                               left_997 <= expected_left_peak * 1.35 &&
                               right_1511 >= expected_right_peak * 0.65 &&
                               right_1511 <= expected_right_peak * 1.35;
        const bool separation_ok = left_1511 < expected_left_peak * 0.05 &&
                                   right_997 < expected_right_peak * 0.05;
        const bool continuity_ok = metrics.silent_frames == 0 &&
                                   metrics.discontinuities == 0 &&
                                   metrics.non_finite_samples == 0;
        if (!capture_rate_ok || !levels_ok || !separation_ok || !continuity_ok)
            throw std::runtime_error("Shared render clients did not produce continuous finite audio with the expected 48kHz capture clock, stereo mix, session gain, and levels");

        std::cout << "PASS: simultaneous shared-mode clients are mixed with expected level and channel identity.\n";
        return 0;
    } catch (const std::exception& e) {
        metrics.measuring.store(false, std::memory_order_relaxed);
        SetEvent(stop.value);
        if (left_thread.joinable()) left_thread.join();
        if (right_thread.joinable()) right_thread.join();
        if (capture_thread.joinable()) capture_thread.join();
        std::cerr << e.what() << '\n';
        return 1;
    }
}
