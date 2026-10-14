#include "axe/asio_host.hpp"
#include "axe/audio_pipeline.hpp"
#include "windows_audio.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <audioclient.h>
#include <endpointvolume.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

namespace {
constexpr double kRate = 48'000.0;
constexpr double kRateTolerance = 0.01;
constexpr std::array<double, 2> kFrequencies{997.0, 1511.0};
constexpr std::array<float, 2> kBaseAmplitudes{0.0005f, 0.00025f};
constexpr std::size_t kPhaseCount = 4;
constexpr std::uint32_t kWindowFrames = 48'000;
constexpr std::uint32_t kWarmupWindows = 2;
constexpr double kMinimumToneRms = 1.0e-8;
constexpr double kToneToBroadbandMinimum = 0.02;
constexpr double kCaptureRmsDriftLimit = 0.15;
constexpr double kOffToOnMaximum = 0.05;
constexpr double kHalfGainTolerance = 0.15;
constexpr std::chrono::seconds kMinimumPhaseDuration{5};
static_assert(std::atomic<float>::is_always_lock_free &&
              std::atomic<std::uint64_t>::is_always_lock_free &&
              std::atomic<std::int32_t>::is_always_lock_free);

std::atomic<bool> gStopRequested{false};

BOOL WINAPI onConsoleControl(DWORD event) noexcept {
    if (event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT ||
        event == CTRL_CLOSE_EVENT || event == CTRL_SHUTDOWN_EVENT) {
        gStopRequested.store(true, std::memory_order_release);
        return TRUE;
    }
    return FALSE;
}

struct StopEvent {
    HANDLE handle{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    StopEvent() {
        if (!handle) throw std::runtime_error("Could not create a worker stop event");
    }
    ~StopEvent() { if (handle) CloseHandle(handle); }
    StopEvent(const StopEvent&) = delete;
    StopEvent& operator=(const StopEvent&) = delete;
};

struct HostWindow {
    HWND handle{CreateWindowExW(0, L"STATIC", L"FAS loopback feasibility test",
                                WS_OVERLAPPED, 0, 0, 0, 0, nullptr, nullptr,
                                GetModuleHandleW(nullptr), nullptr)};
    HostWindow() {
        if (!handle) throw std::runtime_error("Could not create the hidden ASIO host window");
    }
    ~HostWindow() { if (handle) DestroyWindow(handle); }
    HostWindow(const HostWindow&) = delete;
    HostWindow& operator=(const HostWindow&) = delete;
};

struct BridgeMutex {
    HANDLE handle{CreateMutexW(nullptr, TRUE, L"Local\\AxeFxUsb78Bridge")};
    BridgeMutex() {
        if (!handle) throw std::runtime_error("Could not create the single-instance bridge mutex");
        if (GetLastError() == ERROR_ALREADY_EXISTS) {
            CloseHandle(handle);
            handle = nullptr;
            throw std::runtime_error("Another Axe-Fx USB 7/8 bridge or diagnostic is running");
        }
    }
    ~BridgeMutex() {
        if (handle) {
            ReleaseMutex(handle);
            CloseHandle(handle);
        }
    }
    BridgeMutex(const BridgeMutex&) = delete;
    BridgeMutex& operator=(const BridgeMutex&) = delete;
};

void pumpStaMessages() noexcept {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        if (message.message == WM_QUIT) {
            gStopRequested.store(true, std::memory_order_release);
            continue;
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

DWORD waitAndPump(HANDLE stop, DWORD timeoutMilliseconds) noexcept {
    const DWORD result = MsgWaitForMultipleObjectsEx(1, &stop, timeoutMilliseconds,
                                                      QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    pumpStaMessages();
    return result;
}

void require(const axe::AsioHostStatus& status) {
    if (!status) {
        throw std::runtime_error(status.message + " (native=" +
                                 std::to_string(status.nativeCode) + ")");
    }
}

std::wstring widenAscii(std::string_view value) {
    std::wstring result;
    result.reserve(value.size());
    for (const unsigned char character : value) {
        if (character < 0x20 || character > 0x7e) {
            throw std::runtime_error("The Windows endpoint ID must be printable ASCII");
        }
        result.push_back(static_cast<wchar_t>(character));
    }
    return result;
}

double parsePositiveSeconds(std::string_view value) {
    unsigned seconds{};
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), seconds, 10);
    if (value.empty() || parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() ||
        seconds < 30 || seconds > 3600) {
        throw std::runtime_error("--seconds must be an integer from 30 through 3600");
    }
    return static_cast<double>(seconds);
}

std::string printableAscii(std::wstring_view value) {
    std::string result;
    result.reserve(value.size());
    for (wchar_t character : value) {
        if (character < 0x20 || character > 0x7e)
            throw std::runtime_error("Command-line values must be printable ASCII");
        result.push_back(static_cast<char>(character));
    }
    return result;
}

bool parseToneScale(std::string_view value, double& scale) noexcept {
    if (value.empty()) return false;
    const char* first = value.data();
    const char* last = first + value.size();
    const auto parsed = std::from_chars(first, last, scale, std::chars_format::general);
    return parsed.ec == std::errc{} && parsed.ptr == last && std::isfinite(scale) &&
           scale >= 1.0 && scale <= 10.0;
}

DWORD queryEndpointHardwareSupport(const std::wstring& endpointId) {
    const HRESULT initializeResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool ownsApartment = SUCCEEDED(initializeResult);
    if (FAILED(initializeResult) && initializeResult != RPC_E_CHANGED_MODE) {
        axe::win::check(initializeResult, "Initialize endpoint-volume query COM apartment");
    }
    struct ApartmentExit {
        bool owns;
        ~ApartmentExit() { if (owns) CoUninitialize(); }
    } apartmentExit{ownsApartment};

    using Microsoft::WRL::ComPtr;
    ComPtr<IMMDeviceEnumerator> enumerator;
    axe::win::check(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                    IID_PPV_ARGS(&enumerator)), "Create endpoint enumerator");
    ComPtr<IMMDevice> device;
    axe::win::check(enumerator->GetDevice(endpointId.c_str(), &device), "Open selected endpoint");
    ComPtr<IAudioEndpointVolume> endpointVolume;
    axe::win::check(device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
                                    &endpointVolume), "Open read-only endpoint volume metadata");
    DWORD support{};
    axe::win::check(endpointVolume->QueryHardwareSupport(&support), "Query endpoint hardware support");
    return support;
}

void printEndpointHardwareSupport(DWORD support) {
    std::cout << "Endpoint hardware-volume flags=0x" << std::hex << support << std::dec
              << " (volume=" << ((support & ENDPOINT_HARDWARE_SUPPORT_VOLUME) ? "yes" : "no")
              << ", mute=" << ((support & ENDPOINT_HARDWARE_SUPPORT_MUTE) ? "yes" : "no")
              << ", meter=" << ((support & ENDPOINT_HARDWARE_SUPPORT_METER) ? "yes" : "no")
              << "). Read only; endpoint settings are not changed.\n";
}

struct CapturePhaseStats {
    std::uint64_t frames{};
    double leftEnergy{};
    double rightEnergy{};
};

struct ReturnPhaseStats {
    std::uint32_t windows{};
    std::array<double, 2> broadbandRmsSum{};
    std::array<std::array<double, 2>, 2> toneRmsSum{}; // channel, frequency
};

struct ReturnAnalyzer {
    std::int32_t activePhase{-1};
    std::uint32_t windowFrames{};
    std::uint32_t completedWindows{};
    std::array<double, 2> coefficients{};
    std::array<std::array<double, 2>, 2> q1{};
    std::array<std::array<double, 2>, 2> q2{};
    std::array<double, 2> sumSquares{};
    std::array<ReturnPhaseStats, kPhaseCount> phases{};
    std::uint64_t nonFiniteSamples{};

    ReturnAnalyzer() noexcept {
        for (std::size_t tone = 0; tone < kFrequencies.size(); ++tone) {
            const double omega = 2.0 * std::numbers::pi * kFrequencies[tone] / kRate;
            coefficients[tone] = 2.0 * std::cos(omega);
        }
    }

    void resetForPhase(std::int32_t phase) noexcept {
        activePhase = phase;
        windowFrames = 0;
        completedWindows = 0;
        q1 = {};
        q2 = {};
        sumSquares = {};
    }

    void finishWindow() noexcept {
        if (activePhase < 0 || static_cast<std::size_t>(activePhase) >= kPhaseCount) return;
        if (completedWindows >= kWarmupWindows) {
            auto& phase = phases[static_cast<std::size_t>(activePhase)];
            for (std::size_t channel = 0; channel < 2; ++channel) {
                phase.broadbandRmsSum[channel] +=
                    std::sqrt(sumSquares[channel] / static_cast<double>(kWindowFrames));
                for (std::size_t tone = 0; tone < kFrequencies.size(); ++tone) {
                    const double power = std::max(
                        0.0,
                        q1[channel][tone] * q1[channel][tone] +
                            q2[channel][tone] * q2[channel][tone] -
                            coefficients[tone] * q1[channel][tone] * q2[channel][tone]);
                    const double peak = 2.0 * std::sqrt(power) /
                                        static_cast<double>(kWindowFrames);
                    phase.toneRmsSum[channel][tone] += peak / std::sqrt(2.0);
                }
            }
            ++phase.windows;
        }
        ++completedWindows;
        windowFrames = 0;
        q1 = {};
        q2 = {};
        sumSquares = {};
    }

    void consume(std::int32_t phase,
                 const float* left,
                 const float* right,
                 std::uint32_t frames) noexcept {
        if (phase != activePhase) resetForPhase(phase);
        if (phase < 0 || static_cast<std::size_t>(phase) >= kPhaseCount) return;
        for (std::uint32_t frame = 0; frame < frames; ++frame) {
            const std::array<float, 2> raw{left[frame], right[frame]};
            for (std::size_t channel = 0; channel < raw.size(); ++channel) {
                double sample = raw[channel];
                if (!std::isfinite(sample)) {
                    sample = 0.0;
                    ++nonFiniteSamples;
                }
                sumSquares[channel] += sample * sample;
                for (std::size_t tone = 0; tone < kFrequencies.size(); ++tone) {
                    const double next = sample + coefficients[tone] * q1[channel][tone] -
                                        q2[channel][tone];
                    q2[channel][tone] = q1[channel][tone];
                    q1[channel][tone] = next;
                }
            }
            if (++windowFrames == kWindowFrames) finishWindow();
        }
    }
};

struct FeasibilityRun {
    axe::AudioPipeline pipeline{axe::AudioPipelineConfig{
        .input_rate_hz = 48'000,
        .output_rate_hz = 48'000,
        .capacity_frames = 8'192,
        .target_fill_frames = 960,
        .preroll_frames = 960,
        .max_latency_frames = 1'920,
        .max_correction_ppm = 1'500}};
    StopEvent stop;
    StopEvent captureReady;
    axe::AsioHost host;
    std::atomic<float> relayGain{0.0f};
    std::atomic<std::int32_t> phase{-1};
    std::atomic<std::uint64_t> captureFrames{0};
    std::atomic<std::uint64_t> inputFrames{0};
    std::atomic<std::uint64_t> outputFrames{0};
    std::atomic<std::uint64_t> captureDiscontinuities{0};
    std::atomic<std::uint64_t> captureNonFiniteSamples{0};
    std::array<CapturePhaseStats, kPhaseCount> capturePhases{}; // written by capture thread
    ReturnAnalyzer returnAnalyzer; // written by ASIO input callback thread
    std::uint32_t capturePeriod{};
    std::uint32_t captureBufferFrames{};
    HRESULT captureLowLatencyResult{E_NOINTERFACE};
    std::string captureError;
    std::string signalError;
    std::atomic<bool> captureFailed{false};
    std::atomic<bool> signalFailed{false};
    std::thread captureThread;
    std::thread signalThread;
    bool shutdownComplete{false};
    axe::AsioHostStatus stopStatus{};
    axe::AsioHostStatus closeStatus{};

    ~FeasibilityRun() {
        try { shutdown(); } catch (...) {}
    }

    static void onCaptureReady(void* opaque) noexcept {
        auto& self = *static_cast<FeasibilityRun*>(opaque);
        SetEvent(self.captureReady.handle);
    }

    static void onCapture(void* opaque,
                          const float* interleaved,
                          std::uint32_t frames,
                          bool discontinuity) noexcept {
        auto& self = *static_cast<FeasibilityRun*>(opaque);
        self.captureFrames.fetch_add(frames, std::memory_order_relaxed);
        if (discontinuity) {
            self.captureDiscontinuities.fetch_add(1, std::memory_order_relaxed);
            self.pipeline.notify_discontinuity();
        }
        const auto phaseIndex = self.phase.load(std::memory_order_acquire);
        CapturePhaseStats* phaseStats = phaseIndex >= 0 &&
                static_cast<std::size_t>(phaseIndex) < kPhaseCount
            ? &self.capturePhases[static_cast<std::size_t>(phaseIndex)] : nullptr;
        if (phaseStats) phaseStats->frames += frames;

        if (!interleaved) {
            (void)self.pipeline.submit_silence(frames);
            return;
        }
        double leftEnergy = 0.0;
        double rightEnergy = 0.0;
        for (std::uint32_t frame = 0; frame < frames; ++frame) {
            float left = interleaved[2 * frame];
            float right = interleaved[2 * frame + 1];
            if (!std::isfinite(left)) {
                left = 0.0f;
                self.captureNonFiniteSamples.fetch_add(1, std::memory_order_relaxed);
            }
            if (!std::isfinite(right)) {
                right = 0.0f;
                self.captureNonFiniteSamples.fetch_add(1, std::memory_order_relaxed);
            }
            leftEnergy += static_cast<double>(left) * left;
            rightEnergy += static_cast<double>(right) * right;
        }
        if (phaseStats) {
            phaseStats->leftEnergy += leftEnergy;
            phaseStats->rightEnergy += rightEnergy;
        }
        (void)self.pipeline.ingest(interleaved, frames);
    }

    static void onInput(void* opaque,
                        const float* left,
                        const float* right,
                        std::uint32_t frames) noexcept {
        auto& self = *static_cast<FeasibilityRun*>(opaque);
        self.inputFrames.fetch_add(frames, std::memory_order_relaxed);
        const auto phaseIndex = self.phase.load(std::memory_order_acquire);
        self.returnAnalyzer.consume(phaseIndex, left, right, frames);
    }

    static void render(void* opaque,
                       float* left,
                       float* right,
                       std::uint32_t frames) noexcept {
        auto& self = *static_cast<FeasibilityRun*>(opaque);
        self.pipeline.render(left, right, frames);
        const float gain = self.relayGain.load(std::memory_order_relaxed);
        for (std::uint32_t frame = 0; frame < frames; ++frame) {
            left[frame] *= gain;
            right[frame] *= gain;
        }
        self.outputFrames.fetch_add(frames, std::memory_order_relaxed);
    }

    void startCapture(const std::wstring& endpointId) {
        captureThread = std::thread([this, endpointId] {
            try {
                axe::win::capture_loopback(endpointId, stop.handle, &FeasibilityRun::onCapture,
                                           this, capturePeriod, &FeasibilityRun::onCaptureReady,
                                           &captureLowLatencyResult, &captureBufferFrames);
                if (WaitForSingleObject(stop.handle, 0) != WAIT_OBJECT_0) {
                    throw std::runtime_error("WASAPI loopback capture exited before it was stopped");
                }
            } catch (const std::exception& error) {
                captureError = error.what();
                captureFailed.store(true, std::memory_order_release);
                SetEvent(captureReady.handle);
                SetEvent(stop.handle);
            }
        });
    }

    void startSignal(const std::wstring& endpointId,
                     float leftAmplitude,
                     float rightAmplitude) {
        signalThread = std::thread([this, endpointId, leftAmplitude, rightAmplitude] {
            try {
                axe::win::render_test_signal(endpointId, stop.handle,
                                             leftAmplitude, rightAmplitude);
                if (WaitForSingleObject(stop.handle, 0) != WAIT_OBJECT_0) {
                    throw std::runtime_error("WASAPI test source exited before it was stopped");
                }
            } catch (const std::exception& error) {
                signalError = error.what();
                signalFailed.store(true, std::memory_order_release);
                SetEvent(stop.handle);
            }
        });
    }

    void joinWorkers() {
        if (captureThread.joinable()) captureThread.join();
        if (signalThread.joinable()) signalThread.join();
    }

    void shutdown() {
        if (shutdownComplete) return;
        relayGain.store(0.0f, std::memory_order_relaxed);
        phase.store(-1, std::memory_order_release);
        stopStatus = host.stop();
        SetEvent(stop.handle);
        joinWorkers();
        closeStatus = host.close();
        shutdownComplete = true;
    }
};

bool clockRateValid(std::uint64_t frames, double elapsed, double& rate) noexcept {
    rate = elapsed > 0.0 ? static_cast<double>(frames) / elapsed : 0.0;
    return std::isfinite(rate) && rate >= kRate * (1.0 - kRateTolerance) &&
           rate <= kRate * (1.0 + kRateTolerance);
}

double averageTone(const ReturnPhaseStats& phase, std::size_t channel, std::size_t tone) {
    return phase.windows != 0
        ? phase.toneRmsSum[channel][tone] / static_cast<double>(phase.windows) : 0.0;
}

double averageBroadband(const ReturnPhaseStats& phase, std::size_t channel) {
    return phase.windows != 0
        ? phase.broadbandRmsSum[channel] / static_cast<double>(phase.windows) : 0.0;
}

void printAsioChannel(const axe::AsioChannelDescriptor& channel, const char* direction) {
    std::cout << "  ASIO " << direction << ' ' << channel.channelNumber << ": "
              << (channel.name.empty() ? "(unnamed)" : channel.name) << " ["
              << axe::asioSampleFormatName(channel.format) << ", "
              << channel.bytesPerSample << " bytes/sample]\n";
}

void printUsage() {
    std::cout << "Usage: axe-fas-feasibility <exact Speakers (Axe-Fx III) endpoint ID> "
                 "[--seconds N] [--tone-scale 1..10]\n"
                 "Runs four relay phases (off, full, half, off) for at least 30 seconds; "
                 "audio endpoint settings are read only.\n";
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    std::cout << std::unitbuf;
    if (argc < 2) {
        printUsage();
        return 2;
    }
    if (std::wstring_view(argv[1]) == L"--help" || std::wstring_view(argv[1]) == L"-h") {
        printUsage();
        return 0;
    }

    std::wstring endpointId;
    double totalPhaseSeconds = 30.0;
    double toneScale = 1.0;
    bool secondsSpecified = false;
    bool scaleSpecified = false;
    try {
        const std::wstring_view idArgument(argv[1]);
        std::string idAscii;
        idAscii.reserve(idArgument.size());
        for (const wchar_t character : idArgument) {
            if (character < 0x20 || character > 0x7e) {
                throw std::runtime_error("The Windows endpoint ID must be printable ASCII");
            }
            idAscii.push_back(static_cast<char>(character));
        }
        endpointId = widenAscii(idAscii);
        for (int index = 2; index < argc; ++index) {
            const std::wstring_view option(argv[index]);
            if (option == L"--seconds" && index + 1 < argc) {
                if (secondsSpecified) throw std::runtime_error("--seconds may be specified only once");
                const std::wstring value(argv[++index]);
                const auto ascii = printableAscii(value);
                totalPhaseSeconds = parsePositiveSeconds(ascii);
                secondsSpecified = true;
            } else if (option == L"--tone-scale" && index + 1 < argc) {
                if (scaleSpecified) throw std::runtime_error("--tone-scale may be specified only once");
                const std::wstring value(argv[++index]);
                const auto ascii = printableAscii(value);
                if (!parseToneScale(ascii, toneScale)) {
                    throw std::runtime_error("--tone-scale must be finite and from 1 through 10");
                }
                scaleSpecified = true;
            } else {
                printUsage();
                throw std::runtime_error("Unknown or incomplete command-line option");
            }
        }
        if (totalPhaseSeconds / static_cast<double>(kPhaseCount) <
            static_cast<double>(kMinimumPhaseDuration.count())) {
            throw std::runtime_error("The four-phase test requires at least 30 seconds total");
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 2;
    }

    (void)SetConsoleCtrlHandler(&onConsoleControl, TRUE);
    struct ConsoleHandlerOwner {
        ~ConsoleHandlerOwner() { (void)SetConsoleCtrlHandler(&onConsoleControl, FALSE); }
    } consoleHandlerOwner;

    try {
        BridgeMutex mutex;
        const auto endpoints = axe::win::enumerate_render();
        const auto selected = std::find_if(endpoints.begin(), endpoints.end(), [&](const auto& endpoint) {
            return endpoint.id == endpointId;
        });
        if (selected == endpoints.end()) {
            throw std::runtime_error("The exact endpoint ID is not an active Windows render endpoint");
        }
        if (selected->name != L"Speakers (Axe-Fx III)") {
            throw std::runtime_error("The selected endpoint name must be exactly 'Speakers (Axe-Fx III)'; no other endpoint is accepted");
        }
        if (selected->rate != 48'000 || selected->channels != 2 || !selected->ieee_float) {
            throw std::runtime_error("Speakers (Axe-Fx III) must expose shared-mode stereo 48 kHz IEEE float audio");
        }

        axe::win::EndpointVolumeReader endpointVolume(endpointId);
        const auto endpointGains = endpointVolume.gains();
        if (!std::isfinite(endpointGains[0]) || !std::isfinite(endpointGains[1]) ||
            endpointGains[0] <= 0.0f || endpointGains[1] <= 0.0f) {
            throw std::runtime_error("The Axe-Fx Windows endpoint is muted or a channel is at zero. Unmute it in Windows Sound settings and rerun; this test will not change volume.");
        }
        const DWORD endpointHardwareSupport = queryEndpointHardwareSupport(endpointId);

        HostWindow window;
        FeasibilityRun run;
        const axe::AsioInputTap inputTap{&FeasibilityRun::onInput, &run, 0};
        require(run.host.open(&FeasibilityRun::render, &run, window.handle, &inputTap));
        const auto& capabilities = run.host.capabilities();
        if (!capabilities.supports48kHz || capabilities.selectedOutputs[0].channelNumber != 6 ||
            capabilities.selectedOutputs[1].channelNumber != 7 ||
            !capabilities.inputTapEnabled || capabilities.selectedInputs[0].channelNumber != 0 ||
            capabilities.selectedInputs[1].channelNumber != 1) {
            throw std::runtime_error("FAS ASIO did not expose the required 48 kHz USB 7/8 outputs and USB return inputs 0/1");
        }

        std::cout << "Endpoint: " << axe::win::utf8(selected->name)
                  << " (explicit endpoint ID; endpoint/session levels will not be changed)\n"
                  << "WASAPI mix: " << selected->rate << " Hz, " << selected->channels
                  << "ch IEEE float; advertised period=" << selected->period_frames
                  << " frames, minimum=" << selected->minimum_frames << " frames.\n";
        printEndpointHardwareSupport(endpointHardwareSupport);
        std::cout << "Current endpoint channel gains L/R=" << endpointGains[0] << '/'
                  << endpointGains[1] << "; endpoint gain mirror policy="
                  << (endpointVolume.must_mirror_endpoint_gain() ? "apply captured endpoint gains once" :
                                                                  "loopback already contains software gain")
                  << ".\n";
        std::cout << "ASIO driver=" << capabilities.driverName << " version="
                  << capabilities.driverVersion << ", input count=" << capabilities.inputChannelCount
                  << ", output count=" << capabilities.outputChannelCount << ", sample rate="
                  << capabilities.currentSampleRate << " Hz, requested buffer=128 frames, range="
                  << capabilities.minimumBufferFrames << '-' << capabilities.maximumBufferFrames
                  << ", preferred=" << capabilities.preferredBufferFrames
                  << ", granularity=" << capabilities.bufferGranularity << ".\n";
        for (const auto& output : capabilities.selectedOutputs) printAsioChannel(output, "output");
        for (const auto& input : capabilities.selectedInputs) printAsioChannel(input, "input");
        std::cout << "AudioPipeline: target/preroll=960 frames (20 ms), maximum queue=1920 frames (40 ms), capacity=8192 frames.\n"
                  << "Relay output: ASIO USB 7/8. Return tap: ASIO inputs 0/1.\n"
                  << "WASAPI test tones: 997 Hz left, 1511 Hz right; scale=" << toneScale << "x; peak L/R="
                  << kBaseAmplitudes[0] * toneScale << '/' << kBaseAmplitudes[1] * toneScale
                  << " (-" << std::fixed << std::setprecision(2)
                  << -20.0 * std::log10(kBaseAmplitudes[0] * toneScale) << "/-"
                  << -20.0 * std::log10(kBaseAmplitudes[1] * toneScale) << " dBFS).\n";

        run.startCapture(endpointId);
        const auto captureStartDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (WaitForSingleObject(run.captureReady.handle, 0) != WAIT_OBJECT_0) {
            if (gStopRequested.load(std::memory_order_acquire)) return 130;
            if (run.captureFailed.load(std::memory_order_acquire)) {
                throw std::runtime_error("WASAPI loopback startup failed: " + run.captureError);
            }
            if (WaitForSingleObject(run.stop.handle, 0) == WAIT_OBJECT_0) {
                throw std::runtime_error("WASAPI loopback stopped during startup");
            }
            if (std::chrono::steady_clock::now() >= captureStartDeadline) {
                throw std::runtime_error("WASAPI loopback capture did not become ready within 10 seconds");
            }
            (void)waitAndPump(run.stop.handle, 20);
        }
        if (run.captureFailed.load(std::memory_order_acquire)) {
            throw std::runtime_error("WASAPI loopback startup failed: " + run.captureError);
        }

        require(run.host.start(128));
        const float leftAmplitude = kBaseAmplitudes[0] * static_cast<float>(toneScale);
        const float rightAmplitude = kBaseAmplitudes[1] * static_cast<float>(toneScale);
        run.startSignal(endpointId, leftAmplitude, rightAmplitude);
        std::cout << "ASIO active buffer=" << run.host.capabilities().activeBufferFrames
                  << " frames (" << run.host.capabilities().activeBufferFrames / 48.0
                  << " ms), input latency=" << run.host.capabilities().inputLatencyFrames
                  << " frames, output latency=" << run.host.capabilities().outputLatencyFrames
                  << " frames, outputReady="
                  << (run.host.capabilities().outputReadySupported ? "yes" : "no")
                  << "; loopback selected period=" << run.capturePeriod << " frames, buffer="
                  << run.captureBufferFrames << " frames, AudioClient3 init HRESULT=0x"
                  << std::hex << static_cast<std::uint32_t>(run.captureLowLatencyResult) << std::dec
                  << ".\n";

        // Let the shared render source and ring settle before measuring losses.
        const auto settleDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (std::chrono::steady_clock::now() < settleDeadline) {
            if (gStopRequested.load(std::memory_order_acquire)) return 130;
            if (run.captureFailed.load(std::memory_order_acquire) ||
                run.signalFailed.load(std::memory_order_acquire)) {
                throw std::runtime_error(run.captureFailed.load(std::memory_order_acquire)
                    ? "WASAPI capture failed: " + run.captureError
                    : "WASAPI test source failed: " + run.signalError);
            }
            (void)waitAndPump(run.stop.handle, 20);
        }
        if (run.captureFailed.load(std::memory_order_acquire) ||
            run.signalFailed.load(std::memory_order_acquire)) {
            throw std::runtime_error(run.captureFailed.load(std::memory_order_acquire)
                ? "WASAPI capture failed: " + run.captureError
                : "WASAPI test source failed: " + run.signalError);
        }

        const auto pipelineStart = run.pipeline.stats();
        const auto captureDiscontinuitiesStart = run.captureDiscontinuities.load(std::memory_order_relaxed);
        constexpr std::uint32_t fatalNotifications = axe::asio_notify_reset |
            axe::asio_notify_resync | axe::asio_notify_buffer_size_changed |
            axe::asio_notify_sample_rate_changed | axe::asio_notify_overload;
        const auto clockCaptureStart = run.captureFrames.load(std::memory_order_relaxed);
        const auto clockInputStart = run.inputFrames.load(std::memory_order_relaxed);
        const auto clockOutputStart = run.outputFrames.load(std::memory_order_relaxed);
        const auto measurementStart = std::chrono::steady_clock::now();
        const double phaseSeconds = totalPhaseSeconds / static_cast<double>(kPhaseCount);
        constexpr std::array<float, kPhaseCount> relayGains{0.0f, 1.0f, 0.5f, 0.0f};
        constexpr std::array<const char*, kPhaseCount> phaseNames{"relay off", "relay full", "relay half", "relay off again"};

        for (std::size_t phase = 0; phase < kPhaseCount; ++phase) {
            run.relayGain.store(relayGains[phase], std::memory_order_relaxed);
            run.phase.store(static_cast<std::int32_t>(phase), std::memory_order_release);
            const auto phaseStart = std::chrono::steady_clock::now();
            const auto phaseDeadline = phaseStart + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::duration<double>(phaseSeconds));
            std::cout << "Phase " << (phase + 1) << '/' << kPhaseCount << ": "
                      << phaseNames[phase] << ", ASIO relay gain=" << relayGains[phase]
                      << ", duration=" << phaseSeconds << " s.\n";
            while (std::chrono::steady_clock::now() < phaseDeadline) {
                if (gStopRequested.load(std::memory_order_acquire)) {
                    run.phase.store(-1, std::memory_order_release);
                    run.relayGain.store(0.0f, std::memory_order_relaxed);
                    run.shutdown();
                    return 130;
                }
                if (run.captureFailed.load(std::memory_order_acquire) ||
                    run.signalFailed.load(std::memory_order_acquire)) {
                    throw std::runtime_error(run.captureFailed.load(std::memory_order_acquire)
                        ? "WASAPI capture failed: " + run.captureError
                        : "WASAPI test source failed: " + run.signalError);
                }
                const auto notifications = run.host.takeNotifications();
                const auto endpointGain = endpointVolume.gains();
                if (endpointGain[0] == 0.0f || endpointGain[1] == 0.0f) {
                    run.relayGain.store(0.0f, std::memory_order_relaxed);
                    throw std::runtime_error("Endpoint was muted during the feasibility test");
                }
                if ((notifications & fatalNotifications) != 0) {
                    throw std::runtime_error("ASIO reset, overload, resync, buffer-size, or sample-rate notification interrupted the feasibility test");
                }
                if (WaitForSingleObject(run.stop.handle, 0) == WAIT_OBJECT_0) {
                    throw std::runtime_error("A WASAPI worker stopped unexpectedly during a relay phase");
                }
                (void)waitAndPump(run.stop.handle, 20);
            }
            const double actualPhaseSeconds = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - phaseStart).count();
            if (actualPhaseSeconds + 0.001 <
                static_cast<double>(kMinimumPhaseDuration.count())) {
                throw std::runtime_error("A relay phase ended before its 5-second minimum");
            }
        }

        run.phase.store(-1, std::memory_order_release);
        run.relayGain.store(0.0f, std::memory_order_relaxed);
        const auto clockCaptureEnd = run.captureFrames.load(std::memory_order_relaxed);
        const auto clockInputEnd = run.inputFrames.load(std::memory_order_relaxed);
        const auto clockOutputEnd = run.outputFrames.load(std::memory_order_relaxed);
        const auto measurementEnd = std::chrono::steady_clock::now();
        const double elapsed = std::chrono::duration<double>(measurementEnd - measurementStart).count();
        const auto captureFrames = clockCaptureEnd - clockCaptureStart;
        const auto inputFrames = clockInputEnd - clockInputStart;
        const auto outputFrames = clockOutputEnd - clockOutputStart;
        run.shutdown();
        if (!run.stopStatus || !run.closeStatus) {
            throw std::runtime_error("ASIO shutdown failed: " +
                (!run.stopStatus ? run.stopStatus.message : run.closeStatus.message));
        }
        if (run.captureFailed.load(std::memory_order_acquire) ||
            run.signalFailed.load(std::memory_order_acquire)) {
            throw std::runtime_error(run.captureFailed.load(std::memory_order_acquire)
                ? "WASAPI capture failed: " + run.captureError
                : "WASAPI test source failed: " + run.signalError);
        }

        double captureRate{}, inputRate{}, outputRate{};
        const bool captureClockOkay = clockRateValid(captureFrames, elapsed, captureRate);
        const bool inputClockOkay = clockRateValid(inputFrames, elapsed, inputRate);
        const bool outputClockOkay = clockRateValid(outputFrames, elapsed, outputRate);
        std::cout << "Measured clock over " << std::fixed << std::setprecision(3) << elapsed
                  << " seconds: WASAPI loopback=" << captureFrames << " (" << std::setprecision(2)
                  << captureRate << " Hz), ASIO input=" << inputFrames << " (" << inputRate
                  << " Hz), ASIO output=" << outputFrames << " (" << outputRate
                  << " Hz); each must be 48000 Hz +/-1%.\n";

        const auto pipelineEnd = run.pipeline.stats();
        const bool pipelineStable =
            pipelineEnd.dropout_frames == pipelineStart.dropout_frames &&
            pipelineEnd.overflow_frames == pipelineStart.overflow_frames &&
            pipelineEnd.stale_frames_discarded == pipelineStart.stale_frames_discarded &&
            pipelineEnd.discontinuities == pipelineStart.discontinuities &&
            pipelineEnd.non_finite_samples_sanitized == pipelineStart.non_finite_samples_sanitized &&
            run.captureDiscontinuities.load(std::memory_order_relaxed) == captureDiscontinuitiesStart &&
            run.captureNonFiniteSamples.load(std::memory_order_relaxed) == 0 &&
            run.returnAnalyzer.nonFiniteSamples == 0;
        std::cout << "Pipeline after settle: dropout="
                  << (pipelineEnd.dropout_frames - pipelineStart.dropout_frames)
                  << ", overflow=" << (pipelineEnd.overflow_frames - pipelineStart.overflow_frames)
                  << ", stale=" << (pipelineEnd.stale_frames_discarded - pipelineStart.stale_frames_discarded)
                  << ", discontinuities=" << (pipelineEnd.discontinuities - pipelineStart.discontinuities)
                  << ", capture discontinuities=" << (run.captureDiscontinuities.load() - captureDiscontinuitiesStart)
                  << " (startup=" << captureDiscontinuitiesStart << ")"
                  << ", non-finite=" << (pipelineEnd.non_finite_samples_sanitized - pipelineStart.non_finite_samples_sanitized)
                  << ".\n";

        std::array<std::array<double, 2>, kPhaseCount> captureRms{};
        bool capturePhasesValid = true;
        std::cout << std::scientific << std::setprecision(6);
        for (std::size_t phase = 0; phase < kPhaseCount; ++phase) {
            const auto& stats = run.capturePhases[phase];
            if (stats.frames == 0) {
                capturePhasesValid = false;
            } else {
                captureRms[phase][0] = std::sqrt(stats.leftEnergy / static_cast<double>(stats.frames));
                captureRms[phase][1] = std::sqrt(stats.rightEnergy / static_cast<double>(stats.frames));
            }
            std::cout << "Capture " << phaseNames[phase] << " RMS L/R="
                      << captureRms[phase][0] << '/' << captureRms[phase][1]
                      << " over " << stats.frames << " frames.\n";
            if (captureRms[phase][0] < 1.0e-8 || captureRms[phase][1] < 1.0e-8) {
                capturePhasesValid = false;
            }
        }
        const auto captureDrift = [&](std::size_t channel) {
            const auto [minimum, maximum] = std::minmax_element(
                captureRms.begin(), captureRms.end(), [channel](const auto& left, const auto& right) {
                    return left[channel] < right[channel];
                });
            const double mean = (captureRms[0][channel] + captureRms[1][channel] +
                                 captureRms[2][channel] + captureRms[3][channel]) / 4.0;
            return mean > 0.0 ? (maximum->at(channel) - minimum->at(channel)) / mean
                              : std::numeric_limits<double>::infinity();
        };
        const double captureLeftDrift = captureDrift(0);
        const double captureRightDrift = captureDrift(1);
        std::cout << "WASAPI loopback RMS drift across relay states L/R="
                  << captureLeftDrift * 100.0 << "%/" << captureRightDrift * 100.0
                  << "% (limit " << kCaptureRmsDriftLimit * 100.0
                  << "%); loopback source should not change with ASIO relay gain.\n";
        capturePhasesValid = capturePhasesValid &&
            captureLeftDrift <= kCaptureRmsDriftLimit &&
            captureRightDrift <= kCaptureRmsDriftLimit;

        bool returnMetricsValid = true;
        for (std::size_t phase = 0; phase < kPhaseCount; ++phase) {
            const auto& result = run.returnAnalyzer.phases[phase];
            std::cout << "USB return " << phaseNames[phase] << ": measured Goertzel windows="
                      << result.windows << ", broadband RMS L/R="
                      << averageBroadband(result, 0) << '/' << averageBroadband(result, 1)
                      << ", L[997/1511]=" << averageTone(result, 0, 0) << '/'
                      << averageTone(result, 0, 1) << ", R[997/1511]="
                      << averageTone(result, 1, 0) << '/' << averageTone(result, 1, 1) << '\n';
            if (result.windows < 2) returnMetricsValid = false;
        }

        const auto onLeft = averageTone(run.returnAnalyzer.phases[1], 0, 0);
        const auto onRight = averageTone(run.returnAnalyzer.phases[1], 1, 1);
        const auto halfLeft = averageTone(run.returnAnalyzer.phases[2], 0, 0);
        const auto halfRight = averageTone(run.returnAnalyzer.phases[2], 1, 1);
        const auto offFirstLeft = averageTone(run.returnAnalyzer.phases[0], 0, 0);
        const auto offFirstRight = averageTone(run.returnAnalyzer.phases[0], 1, 1);
        const auto offLastLeft = averageTone(run.returnAnalyzer.phases[3], 0, 0);
        const auto offLastRight = averageTone(run.returnAnalyzer.phases[3], 1, 1);
        const auto onLeftCross = averageTone(run.returnAnalyzer.phases[1], 0, 1);
        const auto onRightCross = averageTone(run.returnAnalyzer.phases[1], 1, 0);
        const auto rmsRatio = [](double numerator, double denominator) {
            return denominator > 0.0 ? numerator / denominator : 0.0;
        };
        const double leftHalfRatio = rmsRatio(halfLeft, onLeft);
        const double rightHalfRatio = rmsRatio(halfRight, onRight);
        const double leftOffFirstRatio = rmsRatio(offFirstLeft, onLeft);
        const double rightOffFirstRatio = rmsRatio(offFirstRight, onRight);
        const double leftOffLastRatio = rmsRatio(offLastLeft, onLeft);
        const double rightOffLastRatio = rmsRatio(offLastRight, onRight);
        const bool onTonesDetected = onLeft >= kMinimumToneRms && onRight >= kMinimumToneRms &&
            onLeft >= averageBroadband(run.returnAnalyzer.phases[1], 0) * kToneToBroadbandMinimum &&
            onRight >= averageBroadband(run.returnAnalyzer.phases[1], 1) * kToneToBroadbandMinimum;
        const bool offIsolationOkay = leftOffFirstRatio < kOffToOnMaximum &&
            rightOffFirstRatio < kOffToOnMaximum && leftOffLastRatio < kOffToOnMaximum &&
            rightOffLastRatio < kOffToOnMaximum;
        const bool halfGainOkay = std::abs(leftHalfRatio - 0.5) <= 0.5 * kHalfGainTolerance &&
            std::abs(rightHalfRatio - 0.5) <= 0.5 * kHalfGainTolerance;
        const bool stereoIsolationOkay = onLeftCross <= std::max(onLeft * 0.10, kMinimumToneRms) &&
            onRightCross <= std::max(onRight * 0.10, kMinimumToneRms);
        std::cout << "Relay proof: off/on ratios L/R first=" << leftOffFirstRatio << '/'
                  << rightOffFirstRatio << ", last=" << leftOffLastRatio << '/'
                  << rightOffLastRatio << " (each <5%); half/on=" << leftHalfRatio << '/'
                  << rightHalfRatio << " (0.5 +/-15%); on stereo leakage L1511/R997="
                  << onLeftCross << '/' << onRightCross << ".\n";

        if (gStopRequested.load(std::memory_order_acquire)) return 130;
        if (!captureClockOkay || !inputClockOkay || !outputClockOkay) {
            throw std::runtime_error("One or more audio frame clocks fell outside 48 kHz +/-1%");
        }
        if (!pipelineStable) {
            throw std::runtime_error("Pipeline underflow, overflow, stale frames, discontinuities, or non-finite samples occurred after settling");
        }
        if (!capturePhasesValid) {
            throw std::runtime_error("WASAPI source loopback was silent or its RMS changed with relay state; possible mute, misroute, or feedback");
        }
        if (!returnMetricsValid || !onTonesDetected || !offIsolationOkay ||
            !halfGainOkay || !stereoIsolationOkay) {
            throw std::runtime_error("ASIO USB return did not prove off/full/half/off isolation, expected gain scaling, and stereo tone identity");
        }
        if (run.captureDiscontinuities.load(std::memory_order_relaxed) != captureDiscontinuitiesStart) {
            throw std::runtime_error("WASAPI loopback reported a discontinuity during the test");
        }
        std::cout << "PASS: native Windows loopback relayed only through ASIO USB 7/8 and returned on ASIO inputs 0/1; no feedback detected.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAS loopback feasibility failed: " << error.what() << '\n';
        return 1;
    }
}
