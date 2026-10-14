#include "asio_host_test_hooks.hpp"
#include "axe/asio_host.hpp"

#include <windows.h>
#include <objbase.h>

#include "../third_party/asio/iasiodrv.h"

#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <exception>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

struct FakeState {
    long inputChannels{4};
    long outputChannels{8};
    long minimumFrames{64};
    long maximumFrames{1024};
    long preferredFrames{128};
    long granularity{-1};
    double sampleRate{44100.0};
    ASIOSampleType leftFormat{ASIOSTInt32LSB24};
    ASIOSampleType rightFormat{ASIOSTFloat32LSB};
    std::array<ASIOSampleType, 4> inputFormats{
        ASIOSTInt32LSB24, ASIOSTFloat32LSB, ASIOSTInt16MSB, ASIOSTFloat64MSB};
    std::array<float, 2> inputValues{0.125f, -0.25f};
    bool supports48kHz{true};
    bool failCreate{false};
    bool failStart{false};
    bool failStop{false};
    bool failDispose{false};
    bool emitStartCallbacks{true};
    bool sendResetNotification{true};
    ASIOError outputReadyProbeResult{ASE_NotPresent};
    ASIOError outputReadyCallbackResult{ASE_OK};
    bool latencyChangesAfterOutputReady{false};
    bool latenciesQueriedAfterOutputReady{false};
    bool checkOutputReadyContents{false};
    bool outputReadyProbeAfterBuffers{false};
    bool outputReadyBuffersCorrect{true};
    long outputReadyCalls{0};
    long outputReadyPrefillCalls{0};
    std::uint32_t outputReadyCallbackHalfMask{0};
    long callbackBufferIndex{-1};
    bool validWindow{false};
    long createCalls{0};
    long startCalls{0};
    long stopCalls{0};
    long disposeCalls{0};
    long releaseCalls{0};
    long rateSetCalls{0};
    long buffersRequested{0};
    long mappedChannels[4]{-1, -1, -1, -1};
    ASIOBool mappedInputs[4]{ASIOFalse, ASIOFalse, ASIOFalse, ASIOFalse};
    std::atomic<bool> stopEntered{false};
    ASIOCallbacks callbacks{};
    std::array<std::array<std::vector<std::uint8_t>, 2>, 4> storage;
};

void resetFakeState(FakeState& state) {
    state.inputChannels = 4;
    state.outputChannels = 8;
    state.minimumFrames = 64;
    state.maximumFrames = 1024;
    state.preferredFrames = 128;
    state.granularity = -1;
    state.sampleRate = 44100.0;
    state.leftFormat = ASIOSTInt32LSB24;
    state.rightFormat = ASIOSTFloat32LSB;
    state.inputFormats = {
        ASIOSTInt32LSB24, ASIOSTFloat32LSB, ASIOSTInt16MSB, ASIOSTFloat64MSB};
    state.inputValues = {0.125f, -0.25f};
    state.supports48kHz = true;
    state.failCreate = false;
    state.failStart = false;
    state.failStop = false;
    state.failDispose = false;
    state.emitStartCallbacks = true;
    state.sendResetNotification = true;
    state.outputReadyProbeResult = ASE_NotPresent;
    state.outputReadyCallbackResult = ASE_OK;
    state.latencyChangesAfterOutputReady = false;
    state.latenciesQueriedAfterOutputReady = false;
    state.checkOutputReadyContents = false;
    state.outputReadyProbeAfterBuffers = false;
    state.outputReadyBuffersCorrect = true;
    state.outputReadyCalls = 0;
    state.outputReadyPrefillCalls = 0;
    state.outputReadyCallbackHalfMask = 0;
    state.callbackBufferIndex = -1;
    state.validWindow = false;
    state.createCalls = 0;
    state.startCalls = 0;
    state.stopCalls = 0;
    state.disposeCalls = 0;
    state.releaseCalls = 0;
    state.rateSetCalls = 0;
    state.buffersRequested = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        state.mappedChannels[i] = -1;
        state.mappedInputs[i] = ASIOFalse;
    }
    state.stopEntered.store(false, std::memory_order_relaxed);
    state.callbacks = {};
    for (auto& channel : state.storage) {
        for (auto& half : channel) {
            half.clear();
        }
    }
}

FakeState* gFakeState = nullptr;
bool gFailActivation = false;

class FakeIASIO final : public IASIO {
public:
    explicit FakeIASIO(FakeState& state) noexcept : state_(state) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** result) override {
        if (result == nullptr) {
            return E_POINTER;
        }
        *result = static_cast<IASIO*>(this);
        AddRef();
        return S_OK;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return ++references_;
    }

    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG remaining = --references_;
        if (remaining == 0) {
            ++state_.releaseCalls;
            delete this;
        }
        return remaining;
    }

    ASIOBool init(void* systemWindow) override {
        state_.validWindow = systemWindow != nullptr;
        return state_.validWindow ? ASIOTrue : ASIOFalse;
    }

    void getDriverName(char* name) override {
        constexpr char driverName[] = "Fake FAS ASIO";
        std::memcpy(name, driverName, sizeof(driverName));
    }

    long getDriverVersion() override { return 616; }

    void getErrorMessage(char* message) override {
        constexpr char errorMessage[] = "Fake driver error";
        std::memcpy(message, errorMessage, sizeof(errorMessage));
    }

    ASIOError start() override {
        ++state_.startCalls;
        if (state_.failStart) {
            return ASE_HWMalfunction;
        }
        if (state_.buffersRequested == 4) {
            for (std::size_t descriptor = 0; descriptor < 2; ++descriptor) {
                const auto channel = static_cast<std::size_t>(state_.mappedChannels[descriptor]);
                const auto format = static_cast<axe::AsioSampleFormat>(state_.inputFormats[channel]);
                const auto bytes = axe::asioSampleFormatBytes(format);
                for (auto& half : state_.storage[descriptor]) {
                    for (std::size_t frame = 0; frame < half.size() / bytes; ++frame) {
                        (void)axe::encodeAsioSample(
                            state_.inputValues[descriptor],
                            format,
                            half.data() + frame * bytes);
                    }
                }
            }
        }
        if (state_.sendResetNotification && state_.callbacks.asioMessage != nullptr) {
            (void)state_.callbacks.asioMessage(kAsioResetRequest, 0, nullptr, nullptr);
            (void)state_.callbacks.asioMessage(kAsioResyncRequest, 0, nullptr, nullptr);
            (void)state_.callbacks.asioMessage(kAsioBufferSizeChange, 256, nullptr, nullptr);
        }
        if (state_.emitStartCallbacks && state_.callbacks.bufferSwitchTimeInfo != nullptr) {
            ASIOTime timeInfo{};
            state_.callbackBufferIndex = 0;
            (void)state_.callbacks.bufferSwitchTimeInfo(&timeInfo, 0, ASIOTrue);
            state_.callbackBufferIndex = 1;
            (void)state_.callbacks.bufferSwitchTimeInfo(&timeInfo, 1, ASIOTrue);
            state_.callbackBufferIndex = -1;
        }
        return ASE_OK;
    }

    ASIOError stop() override {
        ++state_.stopCalls;
        state_.stopEntered.store(true, std::memory_order_release);
        return state_.failStop ? ASE_HWMalfunction : ASE_OK;
    }

    ASIOError getChannels(long* inputs, long* outputs) override {
        *inputs = state_.inputChannels;
        *outputs = state_.outputChannels;
        return ASE_OK;
    }

    ASIOError getLatencies(long* input, long* output) override {
        state_.latenciesQueriedAfterOutputReady = state_.outputReadyCalls > 0;
        const bool readyLatency = state_.latencyChangesAfterOutputReady &&
            state_.outputReadyProbeResult == ASE_OK && state_.outputReadyCalls > 0;
        *input = readyLatency ? 96 : 192;
        *output = readyLatency ? 128 : 256;
        return ASE_OK;
    }

    ASIOError getBufferSize(long* minimum, long* maximum,
                            long* preferred, long* granularity) override {
        *minimum = state_.minimumFrames;
        *maximum = state_.maximumFrames;
        *preferred = state_.preferredFrames;
        *granularity = state_.granularity;
        return ASE_OK;
    }

    ASIOError canSampleRate(ASIOSampleRate rate) override {
        return std::abs(rate - 48000.0) < 0.5 && state_.supports48kHz
                   ? ASE_OK
                   : ASE_NoClock;
    }

    ASIOError getSampleRate(ASIOSampleRate* rate) override {
        *rate = state_.sampleRate;
        return ASE_OK;
    }

    ASIOError setSampleRate(ASIOSampleRate rate) override {
        ++state_.rateSetCalls;
        state_.sampleRate = rate;
        return ASE_OK;
    }

    ASIOError getClockSources(ASIOClockSource*, long* count) override {
        *count = 0;
        return ASE_NotPresent;
    }

    ASIOError setClockSource(long) override { return ASE_NotPresent; }

    ASIOError getSamplePosition(ASIOSamples*, ASIOTimeStamp*) override {
        return ASE_NotPresent;
    }

    ASIOError getChannelInfo(ASIOChannelInfo* info) override {
        if (info->isInput == ASIOTrue) {
            if (info->channel < 0 || info->channel >= state_.inputChannels) {
                return ASE_InvalidParameter;
            }
            info->isActive = ASIOFalse;
            info->channelGroup = 0;
            info->type = state_.inputFormats[info->channel];
            const std::string channelName = "USB Return " + std::to_string(info->channel + 1);
            std::memcpy(info->name, channelName.c_str(), channelName.size() + 1);
            return ASE_OK;
        }
        if (info->channel != 6 && info->channel != 7) {
            return ASE_InvalidParameter;
        }
        info->isActive = ASIOFalse;
        info->channelGroup = 0;
        info->type = info->channel == 6 ? state_.leftFormat : state_.rightFormat;
        const char* channelName = info->channel == 6 ? "USB 7" : "USB 8";
        std::memcpy(info->name, channelName, std::strlen(channelName) + 1);
        return ASE_OK;
    }

    ASIOError createBuffers(ASIOBufferInfo* buffers, long count,
                            long frames, ASIOCallbacks* callbacks) override {
        ++state_.createCalls;
        state_.buffersRequested = count;
        state_.callbacks = *callbacks;
        if (count != 2 && count != 4) {
            return ASE_InvalidParameter;
        }
        const long outputOffset = count == 4 ? 2 : 0;
        for (long descriptor = 0; descriptor < count; ++descriptor) {
            state_.mappedChannels[descriptor] = buffers[descriptor].channelNum;
            state_.mappedInputs[descriptor] = buffers[descriptor].isInput;
            if (descriptor < outputOffset && buffers[descriptor].isInput != ASIOTrue) {
                return ASE_InvalidParameter;
            }
            if (descriptor >= outputOffset && buffers[descriptor].isInput != ASIOFalse) {
                return ASE_InvalidParameter;
            }
        }
        if (buffers[outputOffset].channelNum != 6 ||
            buffers[outputOffset + 1].channelNum != 7) {
            return ASE_InvalidParameter;
        }
        if (state_.failCreate) {
            return ASE_NoMemory;
        }

        for (long descriptor = 0; descriptor < count; ++descriptor) {
            ASIOSampleType sampleType{};
            if (buffers[descriptor].isInput == ASIOTrue) {
                const long channel = buffers[descriptor].channelNum;
                if (channel < 0 || channel >= state_.inputChannels) {
                    return ASE_InvalidParameter;
                }
                sampleType = state_.inputFormats[channel];
            } else {
                sampleType = buffers[descriptor].channelNum == 6
                                 ? state_.leftFormat
                                 : state_.rightFormat;
            }
            const std::uint32_t bytes = axe::asioSampleFormatBytes(
                static_cast<axe::AsioSampleFormat>(sampleType));
            if (bytes == 0) {
                return ASE_InvalidParameter;
            }
            for (std::size_t half = 0; half < 2; ++half) {
                auto& data = state_.storage[descriptor][half];
                data.assign(static_cast<std::size_t>(frames) * bytes, 0);
                buffers[descriptor].buffers[half] = data.data();
            }
        }
        return ASE_OK;
    }

    ASIOError disposeBuffers() override {
        ++state_.disposeCalls;
        if (state_.failDispose) {
            return ASE_HWMalfunction;
        }
        for (auto& channel : state_.storage) {
            for (auto& half : channel) {
                half.clear();
            }
        }
        return ASE_OK;
    }

    ASIOError controlPanel() override { return ASE_NotPresent; }
    ASIOError future(long, void*) override { return ASE_NotPresent; }
    ASIOError outputReady() override {
        const long call = ++state_.outputReadyCalls;
        const long outputOffset = state_.buffersRequested == 4 ? 2 : 0;
        const std::size_t half = state_.callbackBufferIndex < 0
                                     ? 1
                                     : static_cast<std::size_t>(state_.callbackBufferIndex);
        if (call == 1) {
            state_.outputReadyProbeAfterBuffers = state_.createCalls > 0 &&
                state_.buffersRequested > 0 &&
                !state_.storage[static_cast<std::size_t>(outputOffset)][1].empty() &&
                !state_.storage[static_cast<std::size_t>(outputOffset + 1)][1].empty();
            ++state_.outputReadyPrefillCalls;
        } else if (state_.callbackBufferIndex >= 0) {
            state_.outputReadyCallbackHalfMask |=
                (std::uint32_t{1} << static_cast<unsigned>(state_.callbackBufferIndex));
        }

        if (state_.checkOutputReadyContents) {
            const auto& left = state_.storage[static_cast<std::size_t>(outputOffset)][half];
            const auto& right = state_.storage[static_cast<std::size_t>(outputOffset + 1)][half];
            const bool leftCorrect = left.size() >= 4 && left[0] == 0x00 &&
                left[1] == 0x00 && left[2] == 0x40 && left[3] == 0x00;
            const bool rightCorrect = right.size() >= 4 && right[0] == 0x00 &&
                right[1] == 0x00 && right[2] == 0x80 && right[3] == 0xbe;
            state_.outputReadyBuffersCorrect = state_.outputReadyBuffersCorrect &&
                leftCorrect && rightCorrect;
        }

        return call == 1 ? state_.outputReadyProbeResult
                         : state_.outputReadyCallbackResult;
    }

private:
    ~FakeIASIO() = default;
    FakeState& state_;
    std::atomic<ULONG> references_{1};
};

long createFakeDriver(void** result) noexcept {
    if (result == nullptr) {
        return E_POINTER;
    }
    *result = nullptr;
    if (gFailActivation || gFakeState == nullptr) {
        return REGDB_E_CLASSNOTREG;
    }
    try {
        *result = static_cast<IASIO*>(new FakeIASIO(*gFakeState));
        return S_OK;
    } catch (...) {
        return E_OUTOFMEMORY;
    }
}

void renderKnown(void*, float* left, float* right, std::uint32_t frames) noexcept {
    for (std::uint32_t i = 0; i < frames; ++i) {
        left[i] = 0.5f;
        right[i] = -0.25f;
    }
}

struct InputCaptureState {
    std::atomic<std::uint64_t> calls{0};
    std::atomic<std::uint64_t> frames{0};
    std::atomic<float> firstLeft{0.0f};
    std::atomic<float> firstRight{0.0f};
};

void captureInputKnown(void* opaque,
                       const float* left,
                       const float* right,
                       std::uint32_t frames) noexcept {
    auto& state = *static_cast<InputCaptureState*>(opaque);
    if (frames > 0) {
        state.firstLeft.store(left[0], std::memory_order_relaxed);
        state.firstRight.store(right[0], std::memory_order_relaxed);
    }
    state.frames.fetch_add(frames, std::memory_order_relaxed);
    state.calls.fetch_add(1, std::memory_order_release);
}

struct DrainState {
    std::atomic<unsigned> calls{0};
    std::atomic<bool> callbackEntered{false};
    std::atomic<bool> releaseCallback{false};
};

void renderUntilReleased(void* opaque, float* left, float* right,
                         std::uint32_t frames) noexcept {
    auto& state = *static_cast<DrainState*>(opaque);
    if (state.calls.fetch_add(1, std::memory_order_relaxed) == 0) {
        for (std::uint32_t i = 0; i < frames; ++i) {
            left[i] = 0.125f;
            right[i] = -0.125f;
        }
        return;
    }
    state.callbackEntered.store(true, std::memory_order_release);
    while (!state.releaseCallback.load(std::memory_order_acquire)) {
        YieldProcessor();
    }
    for (std::uint32_t i = 0; i < frames; ++i) {
        left[i] = 0.125f;
        right[i] = -0.125f;
    }
}

void verifyFormatConversion() {
    std::array<std::uint8_t, 8> bytes{};
    constexpr std::array<axe::AsioSampleFormat, 18> pcmFormats{
        axe::AsioSampleFormat::int16_msb,
        axe::AsioSampleFormat::int24_msb,
        axe::AsioSampleFormat::int32_msb,
        axe::AsioSampleFormat::float32_msb,
        axe::AsioSampleFormat::float64_msb,
        axe::AsioSampleFormat::int32_msb16,
        axe::AsioSampleFormat::int32_msb18,
        axe::AsioSampleFormat::int32_msb20,
        axe::AsioSampleFormat::int32_msb24,
        axe::AsioSampleFormat::int16_lsb,
        axe::AsioSampleFormat::int24_lsb,
        axe::AsioSampleFormat::int32_lsb,
        axe::AsioSampleFormat::float32_lsb,
        axe::AsioSampleFormat::float64_lsb,
        axe::AsioSampleFormat::int32_lsb16,
        axe::AsioSampleFormat::int32_lsb18,
        axe::AsioSampleFormat::int32_lsb20,
        axe::AsioSampleFormat::int32_lsb24,
    };
    for (const auto format : pcmFormats) {
        float decoded{};
        require(axe::encodeAsioSample(0.375f, format, bytes.data()),
                std::string("ASIO encode failed for ") + axe::asioSampleFormatName(format));
        require(axe::decodeAsioSample(bytes.data(), format, decoded),
                std::string("ASIO decode failed for ") + axe::asioSampleFormatName(format));
        require(std::abs(decoded - 0.375f) < 0.000001f,
                std::string("ASIO encode/decode disagreed for ") +
                    axe::asioSampleFormatName(format));
    }

    require(axe::encodeAsioSample(-1.0f, axe::AsioSampleFormat::int32_lsb24,
                                  bytes.data()),
            "Int32LSB24 encoding failed");
    require(bytes[0] == 0x00 && bytes[1] == 0x00 && bytes[2] == 0x80 && bytes[3] == 0xff,
            "Int32LSB24 -1.0 must be right-aligned with sign extension");
    require(axe::encodeAsioSample(1.0f, axe::AsioSampleFormat::int32_lsb24,
                                  bytes.data()),
            "Int32LSB24 positive clamp failed");
    require(bytes[0] == 0xff && bytes[1] == 0xff && bytes[2] == 0x7f && bytes[3] == 0x00,
            "Int32LSB24 +1.0 must clamp to the largest right-aligned positive code");

    require(axe::encodeAsioSample(0.5f, axe::AsioSampleFormat::int32_msb16,
                                  bytes.data()),
            "Int32MSB16 encoding failed");
    require(bytes[0] == 0x00 && bytes[1] == 0x00 && bytes[2] == 0x40 && bytes[3] == 0x00,
            "Int32MSB16 must right-align the 16-bit sample in its 32-bit word");
    require(axe::encodeAsioSample(0.5f, axe::AsioSampleFormat::int32_lsb20,
                                  bytes.data()),
            "Int32LSB20 encoding failed");
    require(bytes[0] == 0x00 && bytes[1] == 0x00 && bytes[2] == 0x04 && bytes[3] == 0x00,
            "Int32LSB20 must right-align the 20-bit sample in its 32-bit word");
    float decoded{};
    require(axe::decodeAsioSample(bytes.data(), axe::AsioSampleFormat::int32_lsb20, decoded) &&
                std::abs(decoded - 0.5f) < 0.00001f,
            "Int32LSB20 decoding must normalize its low 20 valid bits");
    // Independent driver fixtures: upper bits are not part of the N-bit sample.
    bytes = {0x00, 0x00, 0x80, 0x12, 0, 0, 0, 0};
    require(axe::decodeAsioSample(bytes.data(), axe::AsioSampleFormat::int32_lsb24, decoded) && decoded == -1.0f,
            "Int32LSB24 must ignore padding and sign-extend bit 23");
    bytes = {0xab, 0xcd, 0x80, 0x00, 0, 0, 0, 0};
    require(axe::decodeAsioSample(bytes.data(), axe::AsioSampleFormat::int32_msb16, decoded) && decoded == -1.0f,
            "Int32MSB16 must ignore padding and sign-extend bit 15");

    require(axe::encodeAsioSample(-1.0f, axe::AsioSampleFormat::int24_lsb,
                                  bytes.data()),
            "Int24LSB encoding failed");
    require(bytes[0] == 0x00 && bytes[1] == 0x00 && bytes[2] == 0x80,
            "packed Int24LSB -1.0 byte order is wrong");
    require(axe::decodeAsioSample(bytes.data(), axe::AsioSampleFormat::int24_lsb, decoded) &&
                decoded == -1.0f,
            "packed Int24LSB decoding or sign extension is wrong");
    require(axe::encodeAsioSample(0.5f, axe::AsioSampleFormat::int16_msb,
                                  bytes.data()),
            "Int16MSB encoding failed");
    require(bytes[0] == 0x40 && bytes[1] == 0x00,
            "Int16MSB byte order or scaling is wrong");
    require(axe::decodeAsioSample(bytes.data(), axe::AsioSampleFormat::int16_msb, decoded) &&
                std::abs(decoded - 0.5f) < 0.0001f,
            "Int16MSB decoding is wrong");

    require(axe::encodeAsioSample(-0.25f, axe::AsioSampleFormat::float64_msb,
                                  bytes.data()) &&
                axe::decodeAsioSample(bytes.data(), axe::AsioSampleFormat::float64_msb, decoded) &&
                std::abs(decoded + 0.25f) < 0.000001f,
            "Float64MSB byte order or decoding is wrong");

    require(axe::encodeAsioSample(-1.0f, axe::AsioSampleFormat::int32_lsb24,
                                  bytes.data()) &&
                axe::decodeAsioSample(bytes.data(), axe::AsioSampleFormat::int32_lsb24, decoded) &&
                decoded == -1.0f,
            "Int32LSB24 aligned encoding/decoding failed to preserve full scale");

    const float sample = -0.25f;
    require(axe::encodeAsioSample(sample, axe::AsioSampleFormat::float32_lsb,
                                  bytes.data()),
            "Float32LSB encoding failed");
    std::uint32_t expected{};
    std::memcpy(&expected, &sample, sizeof(expected));
    std::uint32_t actual = static_cast<std::uint32_t>(bytes[0]) |
                           (static_cast<std::uint32_t>(bytes[1]) << 8) |
                           (static_cast<std::uint32_t>(bytes[2]) << 16) |
                           (static_cast<std::uint32_t>(bytes[3]) << 24);
    require(actual == expected, "Float32LSB must preserve IEEE-754 little-endian bits");

    require(axe::encodeAsioSample(std::numeric_limits<float>::quiet_NaN(),
                                  axe::AsioSampleFormat::float32_lsb,
                                  bytes.data()),
            "NaN sanitization failed");
    require(bytes[0] == 0 && bytes[1] == 0 && bytes[2] == 0 && bytes[3] == 0,
            "Non-finite samples must be made silent");
    require(!axe::encodeAsioSample(0.0f,
                                   static_cast<axe::AsioSampleFormat>(40),
                                   bytes.data()),
            "DSD sample data must be rejected by the PCM output converter");
}

void verifyMappingCallbacksAndRestoration() {
    FakeState state;
    state.leftFormat = ASIOSTInt32LSB24;
    state.rightFormat = ASIOSTFloat32LSB;
    gFakeState = &state;
    axe::AsioHost host;
    auto status = host.open(&renderKnown, nullptr, GetDesktopWindow());
    require(static_cast<bool>(status), "Fake ASIO driver open failed: " + status.message);
    require(state.validWindow, "ASIO init did not receive the provided HWND");
    require(host.capabilities().outputChannelCount == 8,
            "ASIO output channel count metadata was not read");
    require(host.capabilities().selectedOutputs[0].channelNumber == 6 &&
                host.capabilities().selectedOutputs[1].channelNumber == 7,
            "selected output metadata must name ASIO channels 6 and 7");
    require(host.capabilities().selectedOutputs[0].name == "USB 7" &&
                host.capabilities().selectedOutputs[1].name == "USB 8",
            "selected channel names were not preserved");

    status = host.start(64);
    require(static_cast<bool>(status), "Fake ASIO driver start failed: " + status.message);
    require(state.buffersRequested == 2 && state.mappedChannels[0] == 6 &&
                state.mappedChannels[1] == 7,
            "CreateBuffers must request only physical output channels 6 and 7");
    require(state.sampleRate == 48000.0 && state.rateSetCalls == 1,
            "ASIO start must switch the stream to the fixed 48 kHz clock");
    require(host.capabilities().inputLatencyFrames == 192 &&
                host.capabilities().outputLatencyFrames == 256 &&
                host.capabilities().activeBufferFrames == 64,
            "post-buffer ASIO latency or active buffer metadata is wrong");
    require(host.callbackCount() == 2,
            "both ASIO double-buffer callback indexes must be serviced");

    const auto& left0 = state.storage[0][0];
    const auto& left1 = state.storage[0][1];
    const auto& right0 = state.storage[1][0];
    const auto& right1 = state.storage[1][1];
    for (const auto* buffer : {&left0, &left1}) {
        require(buffer->size() >= 4 && (*buffer)[0] == 0x00 && (*buffer)[1] == 0x00 &&
                    (*buffer)[2] == 0x40 && (*buffer)[3] == 0x00,
                "left PCM samples were not written to the channel-6 driver buffer");
    }
    for (const auto* buffer : {&right0, &right1}) {
        require(buffer->size() >= 4 && (*buffer)[0] == 0x00 && (*buffer)[1] == 0x00 &&
                    (*buffer)[2] == 0x80 && (*buffer)[3] == 0xbe,
                "right float samples were not written to the channel-7 driver buffer");
    }
    require((host.takeNotifications() & (axe::asio_notify_reset |
                                         axe::asio_notify_resync |
                                         axe::asio_notify_buffer_size_changed)) ==
                (axe::asio_notify_reset | axe::asio_notify_resync |
                 axe::asio_notify_buffer_size_changed),
            "ASIO reset, resync, or buffer-size notification was lost");

    const ASIOCallbacks staleCallbacks = state.callbacks;
    status = host.stop();
    require(static_cast<bool>(status), "Fake ASIO driver stop failed: " + status.message);
    const auto beforeStale = host.callbackCount();
    staleCallbacks.bufferSwitch(0, ASIOTrue);
    require(host.callbackCount() == beforeStale,
            "callbacks after stop must not access the stopped host");
    status = host.close();
    require(static_cast<bool>(status), "Fake ASIO driver close failed: " + status.message);
    require(state.disposeCalls == 1 && state.releaseCalls == 1,
            "normal close must dispose buffers then release the driver exactly once");
    require(state.sampleRate == 44100.0 && state.rateSetCalls == 2,
            "normal close must restore the rate that was active before streaming");
}

void verifyOptionalInputTapMappingFormatsAndLifetime() {
    FakeState state;
    state.inputFormats[2] = ASIOSTInt16MSB;
    state.inputFormats[3] = ASIOSTFloat64MSB;
    state.inputValues = {0.375f, -0.625f};
    gFakeState = &state;
    InputCaptureState capture;
    axe::AsioInputTap tap{&captureInputKnown, &capture, 2};
    axe::AsioHost host;
    auto status = host.open(&renderKnown, nullptr, GetDesktopWindow(), &tap);
    require(static_cast<bool>(status), "Fake ASIO input-tap open failed: " + status.message);
    const auto& capabilities = host.capabilities();
    require(capabilities.inputTapEnabled && capabilities.selectedInputFirstChannel == 2 &&
                capabilities.selectedInputs[0].channelNumber == 2 &&
                capabilities.selectedInputs[1].channelNumber == 3,
            "input tap metadata must select only the requested input pair");
    require(capabilities.selectedInputs[0].name == "USB Return 3" &&
                capabilities.selectedInputs[1].name == "USB Return 4" &&
                capabilities.selectedInputs[0].format == axe::AsioSampleFormat::int16_msb &&
                capabilities.selectedInputs[1].format == axe::AsioSampleFormat::float64_msb,
            "selected input names and native formats were not preserved");

    status = host.start(64);
    require(static_cast<bool>(status), "Fake ASIO input-tap start failed: " + status.message);
    require(state.buffersRequested == 4 &&
                state.mappedInputs[0] == ASIOTrue && state.mappedChannels[0] == 2 &&
                state.mappedInputs[1] == ASIOTrue && state.mappedChannels[1] == 3 &&
                state.mappedInputs[2] == ASIOFalse && state.mappedChannels[2] == 6 &&
                state.mappedInputs[3] == ASIOFalse && state.mappedChannels[3] == 7,
            "input tap must open only the selected input pair with output channels 6/7");
    require(capture.calls.load(std::memory_order_acquire) == 2 &&
                capture.frames.load(std::memory_order_relaxed) == 128,
            "input tap must process both driver double-buffer callback indexes");
    require(std::abs(capture.firstLeft.load(std::memory_order_relaxed) - 0.375f) < 0.0001f &&
                std::abs(capture.firstRight.load(std::memory_order_relaxed) + 0.625f) < 0.0001f,
            "input tap did not decode mixed Int16MSB and Float64MSB native formats in channel order");

    const auto& outputLeft = state.storage[2][0];
    const auto& outputRight = state.storage[3][0];
    require(outputLeft.size() >= 4 && outputLeft[0] == 0x00 && outputLeft[1] == 0x00 &&
                outputLeft[2] == 0x40 && outputLeft[3] == 0x00,
            "input descriptors shifted the channel-6 output buffer mapping");
    require(outputRight.size() >= 4 && outputRight[0] == 0x00 && outputRight[1] == 0x00 &&
                outputRight[2] == 0x80 && outputRight[3] == 0xbe,
            "input descriptors shifted the channel-7 output buffer mapping");

    const ASIOCallbacks staleCallbacks = state.callbacks;
    status = host.stop();
    require(static_cast<bool>(status), "Fake ASIO input-tap stop failed");
    const auto captureCallsAfterStop = capture.calls.load(std::memory_order_acquire);
    staleCallbacks.bufferSwitch(0, ASIOTrue);
    require(capture.calls.load(std::memory_order_acquire) == captureCallsAfterStop,
            "input callback must be drained and detached before stop returns");
    status = host.close();
    require(static_cast<bool>(status), "Fake ASIO input-tap close failed");
    staleCallbacks.bufferSwitch(1, ASIOTrue);
    require(capture.calls.load(std::memory_order_acquire) == captureCallsAfterStop,
            "late callbacks after close must not reach the detached input context");

    FakeState firstPair;
    gFakeState = &firstPair;
    InputCaptureState firstPairCapture;
    axe::AsioInputTap firstPairTap{&captureInputKnown, &firstPairCapture, 0};
    axe::AsioHost firstPairHost;
    status = firstPairHost.open(&renderKnown, nullptr, GetDesktopWindow(), &firstPairTap);
    require(static_cast<bool>(status), "ASIO USB Record OUT1 pair open failed");
    status = firstPairHost.start(64);
    require(static_cast<bool>(status), "ASIO USB Record OUT1 pair start failed");
    require(firstPair.buffersRequested == 4 && firstPair.mappedChannels[0] == 0 &&
                firstPair.mappedChannels[1] == 1 && firstPair.mappedInputs[0] == ASIOTrue &&
                firstPair.mappedInputs[1] == ASIOTrue,
            "default ASIO USB Record OUT1 mapping must use input channels 0/1");
    require(std::abs(firstPairCapture.firstLeft.load(std::memory_order_relaxed) - 0.125f) < 0.0001f &&
                std::abs(firstPairCapture.firstRight.load(std::memory_order_relaxed) + 0.25f) < 0.0001f,
            "ASIO USB Record OUT1 input pair decoded incorrectly");
    require(static_cast<bool>(firstPairHost.close()), "ASIO USB Record OUT1 pair close failed");

    FakeState missing;
    missing.inputChannels = 1;
    gFakeState = &missing;
    axe::AsioHost missingHost;
    status = missingHost.open(&renderKnown, nullptr, GetDesktopWindow(), &tap);
    require(!status && status.code == axe::AsioHostError::missing_channels &&
                missing.createCalls == 0 && missing.releaseCalls == 1,
            "missing requested input pair must fail before buffer creation and release the driver");

    FakeState unsupported;
    unsupported.inputFormats[0] = ASIOSTDSDInt8LSB1;
    gFakeState = &unsupported;
    axe::AsioHost unsupportedHost;
    status = unsupportedHost.open(&renderKnown, nullptr, GetDesktopWindow(), &firstPairTap);
    require(!status && status.code == axe::AsioHostError::unsupported_format &&
                unsupported.createCalls == 0 && unsupported.releaseCalls == 1,
            "unsupported DSD input data must be rejected before callback buffer creation");
}

void verifyOutputReadyNegotiationAndFallback() {
    FakeState supported;
    supported.outputReadyProbeResult = ASE_OK;
    supported.latencyChangesAfterOutputReady = true;
    supported.checkOutputReadyContents = true;
    gFakeState = &supported;
    axe::AsioHost host;
    auto status = host.open(&renderKnown, nullptr, GetDesktopWindow());
    require(static_cast<bool>(status), "Fake ASIO open before outputReady test failed");
    status = host.start(64);
    require(static_cast<bool>(status),
            "an accepted outputReady optimization must not prevent ASIO start");
    require(host.capabilities().outputReadySupported,
            "successful initial outputReady must be exposed in ASIO capabilities");
    require(supported.outputReadyCalls == 3 &&
                supported.outputReadyPrefillCalls == 1 &&
                supported.outputReadyCallbackHalfMask == 0x3,
            "outputReady must prime buffer B once, then signal completed buffers 0 and 1");
    require(supported.outputReadyProbeAfterBuffers &&
                supported.outputReadyBuffersCorrect,
            "outputReady must run after buffer creation and after all output samples are written");
    require(supported.latenciesQueriedAfterOutputReady &&
                host.capabilities().inputLatencyFrames == 96 &&
                host.capabilities().outputLatencyFrames == 128,
            "ASIO latencies must be refreshed after outputReady negotiation");

    const ASIOCallbacks staleCallbacks = supported.callbacks;
    const long readyCallsBeforeStop = supported.outputReadyCalls;
    status = host.stop();
    require(static_cast<bool>(status), "outputReady ASIO stop failed");
    staleCallbacks.bufferSwitch(0, ASIOTrue);
    require(supported.outputReadyCalls == readyCallsBeforeStop,
            "a callback after stop must not call outputReady on the detached driver");
    status = host.close();
    require(static_cast<bool>(status), "outputReady ASIO close failed");
    staleCallbacks.bufferSwitch(1, ASIOTrue);
    require(supported.outputReadyCalls == readyCallsBeforeStop,
            "a callback after close must not call outputReady on the released driver");

    FakeState unsupported;
    unsupported.outputReadyProbeResult = ASE_NotPresent;
    unsupported.checkOutputReadyContents = true;
    gFakeState = &unsupported;
    axe::AsioHost fallbackHost;
    status = fallbackHost.open(&renderKnown, nullptr, GetDesktopWindow());
    require(static_cast<bool>(status), "Fake ASIO fallback open failed");
    status = fallbackHost.start(64);
    require(static_cast<bool>(status),
            "an unsupported outputReady probe must retain ordinary ASIO output");
    require(!fallbackHost.capabilities().outputReadySupported &&
                unsupported.outputReadyCalls == 1 &&
                unsupported.outputReadyCallbackHalfMask == 0 &&
                unsupported.outputReadyBuffersCorrect &&
                fallbackHost.callbackCount() == 2,
            "unsupported outputReady must be probed once and must not interrupt either callback half");
    require(static_cast<bool>(fallbackHost.close()), "Fake ASIO fallback close failed");

    FakeState probeError;
    probeError.outputReadyProbeResult = ASE_HWMalfunction;
    gFakeState = &probeError;
    axe::AsioHost errorFallbackHost;
    status = errorFallbackHost.open(&renderKnown, nullptr, GetDesktopWindow());
    require(static_cast<bool>(status), "Fake ASIO probe-error open failed");
    status = errorFallbackHost.start(64);
    require(static_cast<bool>(status) && !errorFallbackHost.capabilities().outputReadySupported &&
                probeError.outputReadyCalls == 1 && errorFallbackHost.callbackCount() == 2,
            "an optional outputReady probe error must fall back without failing streaming");
    require(static_cast<bool>(errorFallbackHost.close()), "Fake ASIO probe-error close failed");

    FakeState callbackError;
    callbackError.outputReadyProbeResult = ASE_OK;
    callbackError.outputReadyCallbackResult = ASE_HWMalfunction;
    gFakeState = &callbackError;
    axe::AsioHost callbackErrorHost;
    status = callbackErrorHost.open(&renderKnown, nullptr, GetDesktopWindow());
    require(static_cast<bool>(status), "Fake ASIO callback-error open failed");
    status = callbackErrorHost.start(64);
    require(static_cast<bool>(status) && callbackErrorHost.callbackCount() == 2 &&
                callbackError.outputReadyCalls == 2,
            "an optional callback-time outputReady error must disable later calls but keep audio running");
    require(static_cast<bool>(callbackErrorHost.close()), "Fake ASIO callback-error close failed");
}

void verifyFailuresAndState() {
    FakeState state;
    gFakeState = &state;
    {
        axe::AsioHost host;
        auto status = host.start();
        require(!status && status.code == axe::AsioHostError::invalid_state,
                "start before open must fail with invalid_state");

        status = host.open(&renderKnown, nullptr, GetDesktopWindow());
        require(static_cast<bool>(status), "Fake ASIO driver open failed: " + status.message);
        std::atomic<axe::AsioHostError> otherThreadError{axe::AsioHostError::none};
        std::thread wrongThread([&] {
            otherThreadError.store(host.start().code, std::memory_order_release);
        });
        wrongThread.join();
        require(otherThreadError.load(std::memory_order_acquire) == axe::AsioHostError::wrong_thread,
                "cross-apartment lifecycle calls must be rejected");
        status = host.start(96);
        require(!status && status.code == axe::AsioHostError::invalid_state,
                "non-power-of-two ASIO buffer request must be rejected");
        require(state.createCalls == 0 && state.rateSetCalls == 0,
                "invalid buffer requests must not touch the driver stream configuration");
        require(static_cast<bool>(host.close()), "close after invalid buffer request failed");
    }

    resetFakeState(state);
    gFakeState = &state;
    state.failCreate = true;
    {
        axe::AsioHost host;
        auto status = host.open(&renderKnown, nullptr, GetDesktopWindow());
        require(static_cast<bool>(status), "open before create failure failed");
        status = host.start(64);
        require(!status && status.code == axe::AsioHostError::driver_call,
                "createBuffers failure must reach the caller");
        require(static_cast<bool>(host.close()), "close after create failure failed");
        require(state.disposeCalls == 1 && state.releaseCalls == 1 &&
                    state.outputReadyCalls == 0,
                "failed createBuffers must dispose/release without probing outputReady on absent buffers");
    }

    resetFakeState(state);
    gFakeState = &state;
    state.failStart = true;
    {
        axe::AsioHost host;
        auto status = host.open(&renderKnown, nullptr, GetDesktopWindow());
        require(static_cast<bool>(status), "open before start failure failed");
        status = host.start(64);
        require(!status && status.code == axe::AsioHostError::driver_call,
                "IASIO::start failure must reach the caller");
        require(state.stopCalls == 1 && state.disposeCalls == 1,
                "start failure must stop and dispose any partially started stream");
        require(static_cast<bool>(host.close()), "close after start failure failed");
        require(state.releaseCalls == 1, "start failure close did not release IASIO");
    }

    resetFakeState(state);
    gFakeState = &state;
    state.supports48kHz = false;
    {
        axe::AsioHost host;
        auto status = host.open(&renderKnown, nullptr, GetDesktopWindow());
        require(static_cast<bool>(status), "open when 48 kHz is unsupported failed unexpectedly");
        require(!host.capabilities().supports48kHz,
                "getCapabilities should report unsupported 48 kHz without changing the device");
        status = host.start(64);
        require(!status && status.code == axe::AsioHostError::unsupported_sample_rate,
                "stream start must refuse a driver that cannot run at 48 kHz");
        require(state.rateSetCalls == 0 && state.createCalls == 0,
                "unsupported 48 kHz must not mutate rate or create buffers");
        require(static_cast<bool>(host.close()), "close after unsupported sample rate failed");
    }

    resetFakeState(state);
    gFakeState = &state;
    state.leftFormat = ASIOSTDSDInt8LSB1;
    {
        axe::AsioHost host;
        const auto status = host.open(&renderKnown, nullptr, GetDesktopWindow());
        require(!status && status.code == axe::AsioHostError::unsupported_format,
                "ASIO DSD must not be interpreted as interleaved PCM");
        require(state.releaseCalls == 1,
                "unsupported output format must release the initialized driver");
    }

    gFailActivation = true;
    {
        axe::AsioHost host;
        const auto status = host.open(&renderKnown, nullptr, GetDesktopWindow());
        require(!status && status.code == axe::AsioHostError::driver_activation,
                "COM activation failure must be reported distinctly");
    }
    gFailActivation = false;
}

void verifyCallbackDrainAndLateDriverCallbackIsolation() {
    FakeState state;
    state.failStop = true;
    state.emitStartCallbacks = false;
    state.sendResetNotification = false;
    gFakeState = &state;
    DrainState drain;
    axe::AsioHost host;
    auto status = host.open(&renderUntilReleased, &drain, GetDesktopWindow());
    require(static_cast<bool>(status), "drain-test ASIO open failed");
    status = host.start(64);
    require(static_cast<bool>(status), "drain-test ASIO start failed");
    const ASIOCallbacks oldDriverCallbacks = state.callbacks;

    std::thread callbackThread([&] {
        oldDriverCallbacks.bufferSwitch(0, ASIOTrue);
    });
    while (!drain.callbackEntered.load(std::memory_order_acquire)) {
        YieldProcessor();
    }
    std::thread callbackReleaseThread([&] {
        while (!state.stopEntered.load(std::memory_order_acquire)) {
            YieldProcessor();
        }
        drain.releaseCallback.store(true, std::memory_order_release);
    });
    status = host.stop();
    callbackThread.join();
    callbackReleaseThread.join();
    require(!status && status.code == axe::AsioHostError::driver_call,
            "failed IASIO::stop must be reported after draining callbacks");
    require(host.callbackCount() == 1,
            "the in-flight callback should complete before stop returns");
    require(static_cast<bool>(host.close()), "drain-test close failed");

    FakeState nextState;
    nextState.emitStartCallbacks = false;
    nextState.sendResetNotification = false;
    gFakeState = &nextState;
    axe::AsioHost nextHost;
    status = nextHost.open(nullptr, nullptr, GetDesktopWindow());
    require(static_cast<bool>(status), "reopened fake ASIO driver failed");
    status = nextHost.start(64);
    require(static_cast<bool>(status), "reopened fake ASIO driver start failed");
    const auto beforeOldCallback = nextHost.callbackCount();
    oldDriverCallbacks.bufferSwitch(0, ASIOTrue);
    require(nextHost.callbackCount() == beforeOldCallback,
            "a late callback from a closed driver must not be routed to the new host");
    require(static_cast<bool>(nextHost.close()), "reopened fake ASIO driver close failed");
}

void verifyRepeatedReconnectsAndSilence() {
    FakeState state;
    state.emitStartCallbacks = true;
    state.sendResetNotification = false;
    gFakeState = &state;
    for (int attempt = 0; attempt < 96; ++attempt) {
        resetFakeState(state);
        state.emitStartCallbacks = true;
        state.sendResetNotification = false;
        gFakeState = &state;
        axe::AsioHost host;
        auto status = host.open(nullptr, nullptr, GetDesktopWindow());
        require(static_cast<bool>(status), "reconnect attempt failed during open: " + status.message);
        status = host.start(64);
        require(static_cast<bool>(status), "reconnect attempt failed during start: " + status.message);
        for (long channelIndex = 0; channelIndex < state.buffersRequested; ++channelIndex) {
            for (const auto& half : state.storage[static_cast<std::size_t>(channelIndex)]) {
                require(half.size() >= 4 && half[0] == 0 && half[1] == 0 &&
                            half[2] == 0 && half[3] == 0,
                        "a missing render callback must leave format-correct silence");
            }
        }
        status = host.close();
        require(static_cast<bool>(status), "reconnect attempt failed during close: " + status.message);
    }
}

}  // namespace

int main() {
    axe::asio_host_internal::setDriverFactoryForTests(&createFakeDriver);
    try {
        verifyFormatConversion();
        verifyMappingCallbacksAndRestoration();
        verifyOptionalInputTapMappingFormatsAndLifetime();
        verifyOutputReadyNegotiationAndFallback();
        verifyFailuresAndState();
        verifyCallbackDrainAndLateDriverCallbackIsolation();
        verifyRepeatedReconnectsAndSilence();
        axe::asio_host_internal::setDriverFactoryForTests(nullptr);
        std::cout << "ASIO sample format and fake-driver lifecycle tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        axe::asio_host_internal::setDriverFactoryForTests(nullptr);
        std::cerr << "ASIO host test failed: " << error.what() << '\n';
        return 1;
    }
}
