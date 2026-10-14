#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>

namespace axe {

// Sample type values follow the Steinberg ASIO 2.3 specification. Keeping the
// enum here lets format conversion be tested without loading a hardware driver.
enum class AsioSampleFormat : std::int32_t {
    int16_msb = 0,
    int24_msb = 1,
    int32_msb = 2,
    float32_msb = 3,
    float64_msb = 4,
    int32_msb16 = 8,
    int32_msb18 = 9,
    int32_msb20 = 10,
    int32_msb24 = 11,
    int16_lsb = 16,
    int24_lsb = 17,
    int32_lsb = 18,
    float32_lsb = 19,
    float64_lsb = 20,
    int32_lsb16 = 24,
    int32_lsb18 = 25,
    int32_lsb20 = 26,
    int32_lsb24 = 27,
};

// Converts one normalized float sample to the byte layout defined by ASIO 2.3.
// Returns false for unsupported (including DSD) formats or a null destination.
bool encodeAsioSample(float sample, AsioSampleFormat format, void* destination) noexcept;

// Decodes one native ASIO PCM sample into normalized float. Reduced-width
// 32-bit formats use right-aligned valid bits and ignore their upper padding.
bool decodeAsioSample(const void* source,
                      AsioSampleFormat format,
                      float& sample) noexcept;

const char* asioSampleFormatName(AsioSampleFormat format) noexcept;
std::uint32_t asioSampleFormatBytes(AsioSampleFormat format) noexcept;

using AsioRenderCallback = void (*)(void* context,
                                    float* left,
                                    float* right,
                                    std::uint32_t frames) noexcept;
using AsioInputTapCallback = void (*)(void* context,
                                      const float* left,
                                      const float* right,
                                      std::uint32_t frames) noexcept;

struct AsioInputTap {
    AsioInputTapCallback callback{nullptr};
    void* callbackContext{nullptr};
    long firstChannel{0};
};

enum class AsioHostError : std::uint8_t {
    none,
    invalid_state,
    com_initialization,
    driver_activation,
    driver_initialization,
    driver_call,
    missing_channels,
    unsupported_format,
    unsupported_sample_rate,
    callback_already_active,
    wrong_thread,
    allocation_failure,
};

struct AsioHostStatus {
    AsioHostError code{AsioHostError::none};
    long nativeCode{0};
    std::string message;

    explicit operator bool() const noexcept { return code == AsioHostError::none; }
};

struct AsioChannelDescriptor {
    // ASIO channel number, zero based.
    std::int32_t channelNumber{-1};
    std::string name;
    AsioSampleFormat format{AsioSampleFormat::int32_lsb};
    std::uint32_t bytesPerSample{0};
    bool formatSupported{false};
};

struct AsioCapabilities {
    std::string driverName;
    long driverVersion{0};
    long inputChannelCount{0};
    long outputChannelCount{0};
    double currentSampleRate{0.0};
    bool supports48kHz{false};
    long minimumBufferFrames{0};
    long maximumBufferFrames{0};
    long preferredBufferFrames{0};
    long bufferGranularity{0};
    long inputLatencyFrames{-1};
    long outputLatencyFrames{-1};
    long activeBufferFrames{0};
    // True when the driver accepted ASIOOutputReady after the first primed
    // output buffer. This is an optional lower-latency path.
    bool outputReadySupported{false};
    std::array<AsioChannelDescriptor, 2> selectedOutputs{};
    bool inputTapEnabled{false};
    long selectedInputFirstChannel{-1};
    std::array<AsioChannelDescriptor, 2> selectedInputs{};
};

enum AsioHostNotification : std::uint32_t {
    asio_notify_none = 0,
    asio_notify_reset = 1u << 0,
    asio_notify_resync = 1u << 1,
    asio_notify_buffer_size_changed = 1u << 2,
    asio_notify_sample_rate_changed = 1u << 3,
    asio_notify_latencies_changed = 1u << 4,
    asio_notify_overload = 1u << 5,
};

// A minimal, single-instance ASIO host for Fractal's installed Axe-Fx III
// driver. Call open/start/stop/close from the same control thread. The render
// callback runs on the driver's real-time callback thread and must be noexcept,
// lock-free, allocation-free, and write all requested frames to both channels.
// An optional input tap opens a pair alongside outputs 6/7 and receives decoded,
// preallocated stereo samples before the render callback. Omitting it preserves
// the two-output-only buffer configuration.
class AsioHost final {
public:
    static constexpr long kSampleRate = 48000;
    static constexpr std::array<long, 2> kOutputChannels{6, 7};

    AsioHost();
    ~AsioHost();

    AsioHost(const AsioHost&) = delete;
    AsioHost& operator=(const AsioHost&) = delete;
    AsioHost(AsioHost&&) = delete;
    AsioHost& operator=(AsioHost&&) = delete;

    // `systemWindowHandle` is the HWND passed to IASIO::init. If omitted, the
    // process console window is used. `callback` may be null for device silence.
    AsioHostStatus open(AsioRenderCallback callback,
                       void* callbackContext,
                       void* systemWindowHandle = nullptr,
                       const AsioInputTap* inputTap = nullptr);
    // Uses the driver's preferred size when bufferFrames is zero; otherwise
    // the requested size must satisfy the driver's reported granularity.
    AsioHostStatus start(long bufferFrames = 0);
    AsioHostStatus stop();
    AsioHostStatus close();

    const AsioCapabilities& capabilities() const noexcept;
    bool isOpen() const noexcept;
    bool isStarted() const noexcept;
    std::uint64_t callbackCount() const noexcept;

    // Notifications are set with atomics on driver callback threads and consumed
    // by the supervisor with one exchange. Handle reset/resync by restarting the
    // host on its control thread.
    std::uint32_t takeNotifications() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace axe
