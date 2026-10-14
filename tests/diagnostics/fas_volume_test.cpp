#include "windows_audio.hpp"

#include <audioclient.h>
#include <endpointvolume.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <exception>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {
using Microsoft::WRL::ComPtr;
constexpr wchar_t kNativeEndpointName[] = L"Speakers (Axe-Fx III)";
constexpr uint64_t kWarmupFrames = 2u * 48000u;
constexpr uint64_t kMeasureFrames = 5u * 48000u;
constexpr uint64_t kPhaseFrames = kWarmupFrames + kMeasureFrames;
constexpr double kMasterMinus6 = 0.5011872336272722;
constexpr double kRightMinus12 = 0.2511886431509580;
std::atomic<HANDLE> g_stop_event{};
std::atomic<bool> g_cancelled{};

BOOL WINAPI handle_console_control(DWORD control) {
    if (control != CTRL_C_EVENT && control != CTRL_BREAK_EVENT) return FALSE;
    g_cancelled.store(true, std::memory_order_relaxed);
    if (const HANDLE event = g_stop_event.load(std::memory_order_relaxed)) SetEvent(event);
    return TRUE;
}

struct ConsoleControlGuard {
    bool installed{};
    explicit ConsoleControlGuard(HANDLE stop) {
        g_stop_event.store(stop, std::memory_order_relaxed);
        installed = SetConsoleCtrlHandler(handle_console_control, TRUE) != FALSE;
        if (!installed) throw std::runtime_error("Could not install Ctrl+C recovery handler");
    }
    ~ConsoleControlGuard() {
        if (installed) SetConsoleCtrlHandler(handle_console_control, FALSE);
        g_stop_event.store(nullptr, std::memory_order_relaxed);
    }
};

struct Handle {
    HANDLE value{};
    ~Handle() { if (value) CloseHandle(value); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    explicit Handle(HANDLE h = nullptr) : value(h) {}
};

struct ComApartment {
    bool owns{};
    ComApartment() {
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) axe::win::check(hr, "Control COM initialization");
        owns = SUCCEEDED(hr);
    }
    ~ComApartment() { if (owns) CoUninitialize(); }
};

struct ErrorState {
    std::mutex mutex;
    std::exception_ptr exception;
    Handle event{CreateEventW(nullptr, TRUE, FALSE, nullptr)};

    void capture_current() noexcept {
        {
            std::lock_guard lock(mutex);
            if (!exception) exception = std::current_exception();
        }
        if (event.value) SetEvent(event.value);
    }
    void rethrow_if_set() {
        if (WaitForSingleObject(event.value, 0) != WAIT_OBJECT_0) return;
        std::exception_ptr saved;
        {
            std::lock_guard lock(mutex);
            saved = exception;
        }
        if (saved) std::rethrow_exception(saved);
        throw std::runtime_error("Audio worker failed without an error detail");
    }
};

struct VolumeSnapshot {
    ComPtr<IAudioEndpointVolume> endpoint;
    float master{};
    float master_db{};
    std::vector<float> channels;
    std::vector<float> channels_db;
    BOOL mute{};
    bool restored{};

    explicit VolumeSnapshot(ComPtr<IAudioEndpointVolume> value) : endpoint(std::move(value)) {
        axe::win::check(endpoint->GetMasterVolumeLevelScalar(&master), "Read original endpoint master level");
        axe::win::check(endpoint->GetMasterVolumeLevel(&master_db), "Read original endpoint master dB");
        UINT count{};
        axe::win::check(endpoint->GetChannelCount(&count), "Read endpoint channel count");
        if (count < 2) throw std::runtime_error("Native Axe-Fx endpoint must expose at least two volume channels");
        channels.resize(count);
        channels_db.resize(count);
        for (UINT i = 0; i < count; ++i) {
            axe::win::check(endpoint->GetChannelVolumeLevelScalar(i, &channels[i]), "Read original endpoint channel level");
            axe::win::check(endpoint->GetChannelVolumeLevel(i, &channels_db[i]), "Read original endpoint channel dB");
        }
        axe::win::check(endpoint->GetMute(&mute), "Read original endpoint mute");
    }

    void restore() {
        if (restored) return;
        axe::win::check(endpoint->SetMute(FALSE, nullptr), "Unmute endpoint before restoring levels");
        axe::win::check(endpoint->SetMasterVolumeLevelScalar(master, nullptr), "Restore endpoint master level");
        for (UINT i = 0; i < channels.size(); ++i)
            axe::win::check(endpoint->SetChannelVolumeLevelScalar(i, channels[i], nullptr), "Restore endpoint channel level");
        axe::win::check(endpoint->SetMute(mute, nullptr), "Restore endpoint mute");
        restored = true;
    }

    ~VolumeSnapshot() {
        if (restored || !endpoint) return;
        endpoint->SetMute(FALSE, nullptr);
        endpoint->SetMasterVolumeLevelScalar(master, nullptr);
        for (UINT i = 0; i < channels.size(); ++i)
            endpoint->SetChannelVolumeLevelScalar(i, channels[i], nullptr);
        endpoint->SetMute(mute, nullptr);
    }

    void set(float master_target_db, std::optional<float> right_channel_db, bool muted) {
        axe::win::check(endpoint->SetMute(FALSE, nullptr), "Unmute endpoint for test phase");
        axe::win::check(endpoint->SetMasterVolumeLevel(master_target_db, nullptr), "Set endpoint test master level");
        UINT count{};
        axe::win::check(endpoint->GetChannelCount(&count), "Read endpoint channel count");
        for (UINT i = 0; i < count; ++i) {
            const float channel_db = i == 1 && right_channel_db ? *right_channel_db : master_target_db;
            axe::win::check(endpoint->SetChannelVolumeLevel(i, channel_db, nullptr),
                            "Set endpoint test channel level");
        }
        axe::win::check(endpoint->SetMute(muted ? TRUE : FALSE, nullptr), "Set endpoint test mute");
    }
};

struct PhaseResult {
    double left_rms{};
    double right_rms{};
    uint64_t measured_frames{};
    uint64_t nonfinite_samples{};
    uint64_t discontinuities{};
    uint64_t silent_frames{};
};

struct CaptureState {
    std::atomic<int> requested_phase{-1};
    std::atomic<int> completed_phase{-1};
    Handle ready_event{CreateEventW(nullptr, FALSE, FALSE, nullptr)};
    Handle phase_event{CreateEventW(nullptr, FALSE, FALSE, nullptr)};
    PhaseResult results[4]{};
};

void capture_ready(void* opaque) {
    auto& state = *static_cast<CaptureState*>(opaque);
    SetEvent(state.ready_event.value);
}

void capture_samples(void* opaque, const float* samples, uint32_t frames, bool discontinuity) {
    struct LocalAccumulator {
        int phase{-1};
        uint64_t phase_frames{};
        uint64_t measured_frames{};
        uint64_t nonfinite_samples{};
        uint64_t discontinuities{};
        uint64_t silent_frames{};
        double left_projection_cos{};
        double left_projection_sin{};
        double right_projection_cos{};
        double right_projection_sin{};
        double left_oscillator_cos{1.0};
        double left_oscillator_sin{};
        double right_oscillator_cos{1.0};
        double right_oscillator_sin{};
        double left_step_cos{};
        double left_step_sin{};
        double right_step_cos{};
        double right_step_sin{};
        bool published{};
    };
    // capture_loopback calls this only from its one capture thread. Keep the
    // accumulator thread-local so the callback does not lock or allocate.
    static thread_local LocalAccumulator acc;
    auto& state = *static_cast<CaptureState*>(opaque);
    const int phase = state.requested_phase.load(std::memory_order_acquire);
    if (phase < 0 || phase >= 4) {
        acc.phase = -1;
        acc.published = false;
        return;
    }
    if (phase != acc.phase) {
        acc = LocalAccumulator{};
        acc.phase = phase;
        constexpr double tau = 6.283185307179586476925286766559;
        acc.left_step_cos = std::cos(tau * 997.0 / 48000.0);
        acc.left_step_sin = std::sin(tau * 997.0 / 48000.0);
        acc.right_step_cos = std::cos(tau * 1511.0 / 48000.0);
        acc.right_step_sin = std::sin(tau * 1511.0 / 48000.0);
    }
    if (acc.published) return;
    if (discontinuity && acc.phase_frames >= kWarmupFrames) ++acc.discontinuities;

    for (uint32_t i = 0; i < frames && acc.phase_frames < kPhaseFrames; ++i, ++acc.phase_frames) {
        if (acc.phase_frames < kWarmupFrames) continue;
        double left = 0.0;
        double right = 0.0;
        if (!samples) {
            ++acc.silent_frames;
        } else {
            left = samples[2u * i];
            right = samples[2u * i + 1u];
            if (!std::isfinite(left)) { left = 0.0; ++acc.nonfinite_samples; }
            if (!std::isfinite(right)) { right = 0.0; ++acc.nonfinite_samples; }
        }
        acc.left_projection_cos += left * acc.left_oscillator_cos;
        acc.left_projection_sin += left * acc.left_oscillator_sin;
        acc.right_projection_cos += right * acc.right_oscillator_cos;
        acc.right_projection_sin += right * acc.right_oscillator_sin;
        const double left_cos = acc.left_oscillator_cos * acc.left_step_cos - acc.left_oscillator_sin * acc.left_step_sin;
        const double left_sin = acc.left_oscillator_sin * acc.left_step_cos + acc.left_oscillator_cos * acc.left_step_sin;
        acc.left_oscillator_cos = left_cos;
        acc.left_oscillator_sin = left_sin;
        const double right_cos = acc.right_oscillator_cos * acc.right_step_cos - acc.right_oscillator_sin * acc.right_step_sin;
        const double right_sin = acc.right_oscillator_sin * acc.right_step_cos + acc.right_oscillator_cos * acc.right_step_sin;
        acc.right_oscillator_cos = right_cos;
        acc.right_oscillator_sin = right_sin;
        ++acc.measured_frames;
    }
    if (acc.phase_frames == kPhaseFrames) {
        auto& result = state.results[phase];
        result.measured_frames = acc.measured_frames;
        result.nonfinite_samples = acc.nonfinite_samples;
        result.discontinuities = acc.discontinuities;
        result.silent_frames = acc.silent_frames;
        if (acc.measured_frames) {
            const double normalization = std::sqrt(2.0) / static_cast<double>(acc.measured_frames);
            result.left_rms = normalization * std::hypot(acc.left_projection_cos, acc.left_projection_sin);
            result.right_rms = normalization * std::hypot(acc.right_projection_cos, acc.right_projection_sin);
        }
        acc.published = true;
        state.completed_phase.store(phase, std::memory_order_release);
        SetEvent(state.phase_event.value);
    }
}

struct Workers {
    HANDLE stop{};
    std::thread capture;
    std::thread render;
    ~Workers() { stop_and_join(); }
    void stop_and_join() noexcept {
        if (stop) SetEvent(stop);
        if (capture.joinable()) capture.join();
        if (render.joinable()) render.join();
    }
};

struct WorkerContext {
    std::wstring endpoint_id;
    HANDLE stop{};
    ErrorState* errors{};
    CaptureState* capture_state{};
    Handle render_entered{CreateEventW(nullptr, FALSE, FALSE, nullptr)};
    uint32_t capture_period{};
};

bool near_ratio(double measured, double expected, double tolerance = 0.15) {
    return std::isfinite(measured) && expected > 0.0 &&
        std::abs(measured - expected) <= expected * tolerance;
}

double ratio(double value, double baseline) {
    return baseline > 0.0 && std::isfinite(value) ? value / baseline : std::numeric_limits<double>::quiet_NaN();
}

std::array<float, 2> expected_gains(axe::win::EndpointVolumeReader& reader) {
    const auto result = reader.gains();
    if (!std::isfinite(result[0]) || !std::isfinite(result[1]) || result[0] < 0 || result[1] < 0)
        throw std::runtime_error("EndpointVolumeReader returned invalid gains");
    return result;
}

void print_phase(const char* name, const PhaseResult& result, const PhaseResult& baseline,
                 std::array<float, 2> gains) {
    std::cout << name << ": raw 997/1511 Hz tone RMS L/R=" << result.left_rms << '/' << result.right_rms
              << ", raw ratio vs baseline L/R=" << ratio(result.left_rms, baseline.left_rms)
              << '/' << ratio(result.right_rms, baseline.right_rms)
              << ", EndpointVolumeReader gains=" << gains[0] << '/' << gains[1]
              << ", expected gained RMS L/R=" << result.left_rms * gains[0] << '/' << result.right_rms * gains[1]
              << ", frames=" << result.measured_frames << ", nonfinite=" << result.nonfinite_samples
              << ", discontinuities=" << result.discontinuities << ", silent_frames=" << result.silent_frames << '\n';
}

void wait_for_workers(HANDLE capture_ready, HANDLE render_entered, HANDLE stop, ErrorState& errors) {
    HANDLE events[] = {capture_ready, render_entered, errors.event.value, stop};
    bool got_capture = false;
    bool got_render = false;
    while (!got_capture || !got_render) {
        DWORD wait = WaitForMultipleObjects(4, events, FALSE, 30000);
        if (wait == WAIT_TIMEOUT) throw std::runtime_error("Timed out waiting for WASAPI workers to start");
        if (wait == WAIT_FAILED) throw std::runtime_error("Worker startup wait failed");
        if (wait == WAIT_OBJECT_0) got_capture = true;
        else if (wait == WAIT_OBJECT_0 + 1) got_render = true;
        else if (wait == WAIT_OBJECT_0 + 2) errors.rethrow_if_set();
        else if (wait == WAIT_OBJECT_0 + 3) throw std::runtime_error("Interrupted by user");
    }
    errors.rethrow_if_set();
}

void wait_for_phase(CaptureState& capture, int phase, HANDLE stop, ErrorState& errors) {
    HANDLE events[] = {capture.phase_event.value, errors.event.value, stop};
    for (;;) {
        DWORD wait = WaitForMultipleObjects(3, events, FALSE, 20000);
        if (wait == WAIT_TIMEOUT) throw std::runtime_error("Timed out collecting five seconds of loopback samples");
        if (wait == WAIT_FAILED) throw std::runtime_error("Volume phase wait failed");
        if (wait == WAIT_OBJECT_0 + 1) errors.rethrow_if_set();
        if (wait == WAIT_OBJECT_0 + 2) throw std::runtime_error("Interrupted by user");
        if (capture.completed_phase.load(std::memory_order_acquire) == phase) return;
    }
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    try {
        if (argc == 2 && std::wstring_view(argv[1]) == L"--help") {
            std::wcout << L"Usage: axe-fas-volume-test --endpoint-id <IMMDevice endpoint ID>\n"
                          L"Tests the exact native Speakers (Axe-Fx III) WASAPI loopback endpoint; no ASIO is opened.\n";
            return 0;
        }
        if (argc != 3 || std::wstring_view(argv[1]) != L"--endpoint-id")
            throw std::runtime_error("Usage: axe-fas-volume-test --endpoint-id <native IMMDevice endpoint ID>");
        const std::wstring endpoint_id(argv[2]);
        ComApartment apartment;
        const auto endpoints = axe::win::enumerate_render();
        const auto found = std::find_if(endpoints.begin(), endpoints.end(), [&](const auto& e) { return e.id == endpoint_id; });
        if (found == endpoints.end()) throw std::runtime_error("The selected endpoint ID is not an active render endpoint");
        if (found->name != kNativeEndpointName)
            throw std::runtime_error("Selected endpoint friendly name must exactly equal 'Speakers (Axe-Fx III)'");
        if (found->rate != 48000 || found->channels != 2 || found->bits != 32 || !found->ieee_float)
            throw std::runtime_error("Native FAS endpoint must expose 48 kHz stereo 32-bit float shared mode");

        ComPtr<IMMDeviceEnumerator> enumerator;
        axe::win::check(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator)), "Endpoint enumerator");
        ComPtr<IMMDevice> device;
        axe::win::check(enumerator->GetDevice(endpoint_id.c_str(), &device), "Native FAS endpoint");
        ComPtr<IAudioEndpointVolume> volume;
        axe::win::check(device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, &volume), "Endpoint volume controls");
        DWORD hardware_support{};
        axe::win::check(volume->QueryHardwareSupport(&hardware_support), "Endpoint volume hardware support");
        VolumeSnapshot snapshot(volume);
        axe::win::EndpointVolumeReader gain_reader(endpoint_id);

        std::cout << std::fixed << std::setprecision(9)
                  << "Native endpoint: " << axe::win::utf8(found->name) << "\n"
                  << "Endpoint ID: " << axe::win::utf8(endpoint_id) << "\n"
                  << "Mix format: " << found->rate << " Hz, " << found->channels << " channels, "
                  << found->bits << "-bit " << (found->ieee_float ? "float" : "integer") << "\n"
                  << "Shared period: default " << found->period_frames << " frames, minimum " << found->minimum_frames << " frames\n"
                  << "Endpoint hardware support flags: 0x" << std::hex << hardware_support << std::dec
                  << " (volume=" << ((hardware_support & ENDPOINT_HARDWARE_SUPPORT_VOLUME) ? "yes" : "no")
                  << ", mute=" << ((hardware_support & ENDPOINT_HARDWARE_SUPPORT_MUTE) ? "yes" : "no") << ")\n"
                  << "Original endpoint volume: master=" << snapshot.master_db << " dB (scalar " << snapshot.master << ")"
                  << ", channels dB/scalar=";
        for (size_t i = 0; i < snapshot.channels.size(); ++i) {
            if (i) std::cout << ", ";
            std::cout << snapshot.channels_db[i] << '/' << snapshot.channels[i];
        }
        std::cout << ", mute=" << (snapshot.mute ? "on" : "off") << '\n';

        Handle stop{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
        if (!stop.value)
            throw std::runtime_error("Could not create diagnostic synchronization events");
        ConsoleControlGuard console_control(stop.value);
        ErrorState errors;
        CaptureState capture_state;
        WorkerContext context{endpoint_id, stop.value, &errors, &capture_state,
                              Handle{CreateEventW(nullptr, FALSE, FALSE, nullptr)}, 0};
        if (!errors.event.value || !capture_state.ready_event.value || !capture_state.phase_event.value ||
            !context.render_entered.value)
            throw std::runtime_error("Could not create diagnostic synchronization events");
        Workers workers;
        workers.stop = stop.value;
        workers.capture = std::thread([&] {
            try {
                axe::win::capture_loopback(endpoint_id, stop.value, capture_samples, &capture_state,
                                           context.capture_period, capture_ready, nullptr, nullptr);
                if (WaitForSingleObject(stop.value, 0) != WAIT_OBJECT_0)
                    throw std::runtime_error("WASAPI loopback stopped unexpectedly");
            } catch (...) {
                errors.capture_current();
                SetEvent(stop.value);
                SetEvent(capture_state.ready_event.value);
                SetEvent(capture_state.phase_event.value);
            }
        });
        workers.render = std::thread([&] {
            SetEvent(context.render_entered.value);
            try {
                axe::win::render_test_signal(endpoint_id, stop.value);
            } catch (...) {
                errors.capture_current();
                SetEvent(stop.value);
                SetEvent(capture_state.ready_event.value);
                SetEvent(capture_state.phase_event.value);
            }
        });
        wait_for_workers(capture_state.ready_event.value, context.render_entered.value, stop.value, errors);

        std::array<PhaseResult, 4> phases{};
        std::array<std::array<float, 2>, 4> observed_gains{};
        const char* names[] = {"baseline 0 dB", "master -6 dB", "master -6 dB + right -12 dB", "mute"};
        for (int phase = 0; phase < 4; ++phase) {
            switch (phase) {
                case 0: snapshot.set(0.0f, std::nullopt, false); break;
                case 1: snapshot.set(-6.0f, std::nullopt, false); break;
                case 2: snapshot.set(-6.0f, -12.0f, false); break;
                case 3: snapshot.set(0.0f, std::nullopt, true); break;
            }
            ResetEvent(capture_state.phase_event.value);
            capture_state.requested_phase.store(phase, std::memory_order_release);
            observed_gains[phase] = expected_gains(gain_reader);
            wait_for_phase(capture_state, phase, stop.value, errors);
            phases[phase] = capture_state.results[phase];
            print_phase(names[phase], phases[phase], phases[0], observed_gains[phase]);
        }

        capture_state.requested_phase.store(-1, std::memory_order_release);
        workers.stop_and_join();
        errors.rethrow_if_set();

        const auto& baseline = phases[0];
        const double master_left = ratio(phases[1].left_rms, baseline.left_rms);
        const double master_right = ratio(phases[1].right_rms, baseline.right_rms);
        const double balance_left = ratio(phases[2].left_rms, baseline.left_rms);
        const double balance_right = ratio(phases[2].right_rms, baseline.right_rms);
        const double mute_left = ratio(phases[3].left_rms, baseline.left_rms);
        const double mute_right = ratio(phases[3].right_rms, baseline.right_rms);

        const bool raw_pre_volume = near_ratio(master_left, 1.0) && near_ratio(master_right, 1.0) &&
                                    near_ratio(balance_left, 1.0) && near_ratio(balance_right, 1.0);
        const bool raw_post_volume = near_ratio(master_left, kMasterMinus6) && near_ratio(master_right, kMasterMinus6) &&
                                     near_ratio(balance_left, kMasterMinus6) && near_ratio(balance_right, kRightMinus12);
        const bool mute_was_applied = std::isfinite(mute_left) && std::isfinite(mute_right) &&
                                      mute_left <= 0.01 && mute_right <= 0.01;
        const bool mute_was_bypassed = near_ratio(mute_left, 1.0) && near_ratio(mute_right, 1.0);
        const bool expected_to_mirror = raw_pre_volume;
        const float master_gain_expected = expected_to_mirror ? static_cast<float>(kMasterMinus6) : 1.0f;
        const float right_gain_expected = expected_to_mirror ? static_cast<float>(kRightMinus12) : 1.0f;
        const bool gains_match = near_ratio(observed_gains[0][0], 1.0) &&
                                 near_ratio(observed_gains[0][1], 1.0) &&
                                 near_ratio(observed_gains[1][0], master_gain_expected) &&
                                 near_ratio(observed_gains[1][1], master_gain_expected) &&
                                 near_ratio(observed_gains[2][0], master_gain_expected) &&
                                 near_ratio(observed_gains[2][1], right_gain_expected) &&
                                 observed_gains[3][0] == 0.0f && observed_gains[3][1] == 0.0f;
        const bool valid_measurements = baseline.left_rms > 1.0e-8 && baseline.right_rms > 1.0e-8 &&
            std::all_of(std::begin(phases), std::end(phases), [](const PhaseResult& p) {
                return p.measured_frames == kMeasureFrames && p.nonfinite_samples == 0 && p.discontinuities == 0 &&
                       std::isfinite(p.left_rms) && std::isfinite(p.right_rms);
            });

        std::cout << "Raw master ratios L/R: " << master_left << '/' << master_right << "\n"
                  << "Raw balance ratios L/R: " << balance_left << '/' << balance_right << "\n"
                  << "Raw mute ratios L/R: " << mute_left << '/' << mute_right << "\n"
                  << "Loopback gain tap: " << (raw_pre_volume ? "pre-volume" : raw_post_volume ? "post-volume" : "ambiguous/mixed") << "\n"
                  << "Loopback mute tap: " << (mute_was_applied ? "mute included" : mute_was_bypassed ? "mute bypassed" : "ambiguous") << "\n"
                  << "EndpointVolumeReader policy: " << (gain_reader.must_mirror_endpoint_gain() ? "mirror channel gains; always mirror mute" : "unity channel gains; always mirror mute") << "\n"
                  << "Capture engine period: " << context.capture_period << " frames\n";

        const bool classified = raw_pre_volume || raw_post_volume;
        const bool policy_correct = (raw_pre_volume && gain_reader.must_mirror_endpoint_gain()) ||
                                    (raw_post_volume && !gain_reader.must_mirror_endpoint_gain());
        snapshot.restore();
        std::cout << "Original endpoint master, all channel levels, and mute restored.\n";
        if (!valid_measurements) throw std::runtime_error("Invalid capture: expected clean five-second windows with finite stereo audio");
        if (!classified) throw std::runtime_error("Volume response is mixed or ambiguous; do not infer gain handling");
        if (!mute_was_applied && !mute_was_bypassed) throw std::runtime_error("Mute response is ambiguous");
        if (!gains_match) throw std::runtime_error("EndpointVolumeReader gains do not match the expected master/balance controls");
        if (!policy_correct) throw std::runtime_error("Current mirrored-gain policy would double-apply the native endpoint's captured attenuation");
        std::cout << "PASS: native FAS loopback is " << (raw_pre_volume ? "pre-volume" : "post-volume")
                  << "; EndpointVolumeReader gain policy is consistent with the measured tap.\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return g_cancelled.load(std::memory_order_relaxed) ? 130 : 1;
    }
}
