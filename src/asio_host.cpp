#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objbase.h>

#include "axe/asio_host.hpp"
#include "iasiodrv.h"
#if defined(AXE_ASIO_TESTING)
#include "asio_host_test_hooks.hpp"
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <new>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace axe {
namespace asio_host_internal {
#if defined(AXE_ASIO_TESTING)
namespace {
std::atomic<DriverFactoryForTests> gDriverFactoryForTests{nullptr};
}

void setDriverFactoryForTests(DriverFactoryForTests factory) noexcept {
    gDriverFactoryForTests.store(factory, std::memory_order_release);
}
#endif
}  // namespace asio_host_internal

namespace {

constexpr GUID kFractalAxeFxIiiAsioClsid{
    0x4fe069b2, 0xb385, 0x48b4, {0x8a, 0x92, 0xb9, 0xa3, 0x5b, 0x6f, 0xfa, 0x0f}};
constexpr double kTargetRate = 48000.0;
constexpr double kRateTolerance = 0.5;

float finiteOrZero(float sample) noexcept {
    return std::isfinite(sample) ? sample : 0.0f;
}

std::int64_t quantizeSigned(float sample, unsigned bits) noexcept {
    const auto scale = static_cast<double>(std::uint64_t{1} << (bits - 1));
    const std::int64_t minimum = -(std::int64_t{1} << (bits - 1));
    const std::int64_t maximum = (std::int64_t{1} << (bits - 1)) - 1;
    const double input = static_cast<double>(finiteOrZero(sample));

    if (input <= -1.0) {
        return minimum;
    }
    if (input >= 1.0) {
        return maximum;
    }

    // Round to nearest while reserving the extra negative two's-complement code.
    const auto rounded = static_cast<std::int64_t>(std::llround(input * scale));
    return std::clamp(rounded, minimum, maximum);
}

void write16(void* destination, std::int16_t value, bool bigEndian) noexcept {
    auto* bytes = static_cast<std::uint8_t*>(destination);
    const auto word = static_cast<std::uint16_t>(value);
    if (bigEndian) {
        bytes[0] = static_cast<std::uint8_t>(word >> 8);
        bytes[1] = static_cast<std::uint8_t>(word);
    } else {
        bytes[0] = static_cast<std::uint8_t>(word);
        bytes[1] = static_cast<std::uint8_t>(word >> 8);
    }
}

void write24(void* destination, std::int32_t value, bool bigEndian) noexcept {
    auto* bytes = static_cast<std::uint8_t*>(destination);
    const auto word = static_cast<std::uint32_t>(value);
    if (bigEndian) {
        bytes[0] = static_cast<std::uint8_t>(word >> 16);
        bytes[1] = static_cast<std::uint8_t>(word >> 8);
        bytes[2] = static_cast<std::uint8_t>(word);
    } else {
        bytes[0] = static_cast<std::uint8_t>(word);
        bytes[1] = static_cast<std::uint8_t>(word >> 8);
        bytes[2] = static_cast<std::uint8_t>(word >> 16);
    }
}

void write32(void* destination, std::int32_t value, bool bigEndian) noexcept {
    auto* bytes = static_cast<std::uint8_t*>(destination);
    const auto word = static_cast<std::uint32_t>(value);
    if (bigEndian) {
        bytes[0] = static_cast<std::uint8_t>(word >> 24);
        bytes[1] = static_cast<std::uint8_t>(word >> 16);
        bytes[2] = static_cast<std::uint8_t>(word >> 8);
        bytes[3] = static_cast<std::uint8_t>(word);
    } else {
        bytes[0] = static_cast<std::uint8_t>(word);
        bytes[1] = static_cast<std::uint8_t>(word >> 8);
        bytes[2] = static_cast<std::uint8_t>(word >> 16);
        bytes[3] = static_cast<std::uint8_t>(word >> 24);
    }
}

void write64Float(void* destination, double value, bool bigEndian) noexcept {
    std::uint64_t word{};
    std::memcpy(&word, &value, sizeof(word));
    auto* bytes = static_cast<std::uint8_t*>(destination);
    for (std::size_t i = 0; i < sizeof(word); ++i) {
        const auto shift = static_cast<unsigned>((bigEndian ? 7 - i : i) * 8);
        bytes[i] = static_cast<std::uint8_t>(word >> shift);
    }
}

std::uint32_t readWord(const void* source, std::size_t bytes, bool bigEndian) noexcept {
    const auto* data = static_cast<const std::uint8_t*>(source);
    std::uint32_t word = 0;
    if (bigEndian) {
        for (std::size_t i = 0; i < bytes; ++i) {
            word = (word << 8) | data[i];
        }
    } else {
        for (std::size_t i = 0; i < bytes; ++i) {
            word |= static_cast<std::uint32_t>(data[i]) << (i * 8);
        }
    }
    return word;
}

std::int32_t signExtend(std::uint32_t word, unsigned bits) noexcept {
    if (bits < 32) {
        word &= (std::uint32_t{1} << bits) - 1;
        if ((word & (std::uint32_t{1} << (bits - 1))) != 0)
            word |= (~std::uint32_t{0}) << bits;
    }
    std::int32_t result{};
    std::memcpy(&result, &word, sizeof(result));
    return result;
}

template <AsioSampleFormat Format>
bool decodeSampleTyped(const void* source, float& sample) noexcept {
    if (source == nullptr) {
        sample = 0.0f;
        return false;
    }

    constexpr bool bigEndian =
        Format == AsioSampleFormat::int16_msb || Format == AsioSampleFormat::int24_msb ||
        Format == AsioSampleFormat::int32_msb || Format == AsioSampleFormat::float32_msb ||
        Format == AsioSampleFormat::float64_msb || Format == AsioSampleFormat::int32_msb16 ||
        Format == AsioSampleFormat::int32_msb18 || Format == AsioSampleFormat::int32_msb20 ||
        Format == AsioSampleFormat::int32_msb24;

    if constexpr (Format == AsioSampleFormat::int16_msb ||
                  Format == AsioSampleFormat::int16_lsb) {
        const auto value = signExtend(readWord(source, 2, bigEndian), 16);
        sample = static_cast<float>(static_cast<double>(value) / 32768.0);
        return true;
    } else if constexpr (Format == AsioSampleFormat::int24_msb ||
                         Format == AsioSampleFormat::int24_lsb) {
        const auto value = signExtend(readWord(source, 3, bigEndian), 24);
        sample = static_cast<float>(static_cast<double>(value) / 8388608.0);
        return true;
    } else if constexpr (Format == AsioSampleFormat::int32_msb ||
                         Format == AsioSampleFormat::int32_lsb) {
        const auto value = signExtend(readWord(source, 4, bigEndian), 32);
        sample = static_cast<float>(static_cast<double>(value) / 2147483648.0);
        return true;
    } else if constexpr (Format == AsioSampleFormat::int32_msb16 ||
                         Format == AsioSampleFormat::int32_lsb16) {
        const auto value = signExtend(readWord(source, 4, bigEndian), 16);
        sample = static_cast<float>(static_cast<double>(value) / 32768.0);
        return true;
    } else if constexpr (Format == AsioSampleFormat::int32_msb18 ||
                         Format == AsioSampleFormat::int32_lsb18) {
        const auto value = signExtend(readWord(source, 4, bigEndian), 18);
        sample = static_cast<float>(static_cast<double>(value) / 131072.0);
        return true;
    } else if constexpr (Format == AsioSampleFormat::int32_msb20 ||
                         Format == AsioSampleFormat::int32_lsb20) {
        const auto value = signExtend(readWord(source, 4, bigEndian), 20);
        sample = static_cast<float>(static_cast<double>(value) / 524288.0);
        return true;
    } else if constexpr (Format == AsioSampleFormat::int32_msb24 ||
                         Format == AsioSampleFormat::int32_lsb24) {
        const auto value = signExtend(readWord(source, 4, bigEndian), 24);
        sample = static_cast<float>(static_cast<double>(value) / 8388608.0);
        return true;
    } else if constexpr (Format == AsioSampleFormat::float32_msb ||
                         Format == AsioSampleFormat::float32_lsb) {
        const auto word = readWord(source, 4, bigEndian);
        std::memcpy(&sample, &word, sizeof(sample));
        sample = finiteOrZero(sample);
        return true;
    } else if constexpr (Format == AsioSampleFormat::float64_msb ||
                         Format == AsioSampleFormat::float64_lsb) {
        const auto* data = static_cast<const std::uint8_t*>(source);
        std::uint64_t word = 0;
        if (bigEndian) {
            for (std::size_t i = 0; i < sizeof(word); ++i) {
                word = (word << 8) | data[i];
            }
        } else {
            for (std::size_t i = 0; i < sizeof(word); ++i) {
                word |= static_cast<std::uint64_t>(data[i]) << (i * 8);
            }
        }
        double value{};
        std::memcpy(&value, &word, sizeof(value));
        sample = finiteOrZero(static_cast<float>(value));
        return true;
    } else {
        sample = 0.0f;
        return false;
    }
}

template <AsioSampleFormat Format>
void decodeSampleBufferTyped(const std::uint8_t* source,
                             float* destination,
                             std::size_t frames,
                             std::uint32_t bytesPerSample) noexcept {
    for (std::size_t frame = 0; frame < frames; ++frame) {
        (void)decodeSampleTyped<Format>(source + frame * bytesPerSample,
                                        destination[frame]);
    }
}

void decodeSampleBuffer(const std::uint8_t* source,
                        float* destination,
                        std::size_t frames,
                        std::uint32_t bytesPerSample,
                        AsioSampleFormat format) noexcept {
    switch (format) {
#define AXE_DECODE_SAMPLE_BUFFER_CASE(formatName) \
    case AsioSampleFormat::formatName: \
        decodeSampleBufferTyped<AsioSampleFormat::formatName>( \
            source, destination, frames, bytesPerSample); \
        return
        AXE_DECODE_SAMPLE_BUFFER_CASE(int16_msb);
        AXE_DECODE_SAMPLE_BUFFER_CASE(int24_msb);
        AXE_DECODE_SAMPLE_BUFFER_CASE(int32_msb);
        AXE_DECODE_SAMPLE_BUFFER_CASE(float32_msb);
        AXE_DECODE_SAMPLE_BUFFER_CASE(float64_msb);
        AXE_DECODE_SAMPLE_BUFFER_CASE(int32_msb16);
        AXE_DECODE_SAMPLE_BUFFER_CASE(int32_msb18);
        AXE_DECODE_SAMPLE_BUFFER_CASE(int32_msb20);
        AXE_DECODE_SAMPLE_BUFFER_CASE(int32_msb24);
        AXE_DECODE_SAMPLE_BUFFER_CASE(int16_lsb);
        AXE_DECODE_SAMPLE_BUFFER_CASE(int24_lsb);
        AXE_DECODE_SAMPLE_BUFFER_CASE(int32_lsb);
        AXE_DECODE_SAMPLE_BUFFER_CASE(float32_lsb);
        AXE_DECODE_SAMPLE_BUFFER_CASE(float64_lsb);
        AXE_DECODE_SAMPLE_BUFFER_CASE(int32_lsb16);
        AXE_DECODE_SAMPLE_BUFFER_CASE(int32_lsb18);
        AXE_DECODE_SAMPLE_BUFFER_CASE(int32_lsb20);
        AXE_DECODE_SAMPLE_BUFFER_CASE(int32_lsb24);
#undef AXE_DECODE_SAMPLE_BUFFER_CASE
        default:
            std::fill_n(destination, frames, 0.0f);
            return;
    }
}

template <AsioSampleFormat Format>
bool encodeSampleTyped(float sample, void* destination) noexcept {
    if (destination == nullptr) {
        return false;
    }

    constexpr bool bigEndian =
        Format == AsioSampleFormat::int16_msb || Format == AsioSampleFormat::int24_msb ||
        Format == AsioSampleFormat::int32_msb || Format == AsioSampleFormat::float32_msb ||
        Format == AsioSampleFormat::float64_msb || Format == AsioSampleFormat::int32_msb16 ||
        Format == AsioSampleFormat::int32_msb18 || Format == AsioSampleFormat::int32_msb20 ||
        Format == AsioSampleFormat::int32_msb24;

    if constexpr (Format == AsioSampleFormat::int16_msb ||
                  Format == AsioSampleFormat::int16_lsb) {
        write16(destination,
                static_cast<std::int16_t>(quantizeSigned(sample, 16)),
                bigEndian);
        return true;
    } else if constexpr (Format == AsioSampleFormat::int24_msb ||
                         Format == AsioSampleFormat::int24_lsb) {
        write24(destination,
                static_cast<std::int32_t>(quantizeSigned(sample, 24)),
                bigEndian);
        return true;
    } else if constexpr (Format == AsioSampleFormat::int32_msb ||
                         Format == AsioSampleFormat::int32_lsb) {
        write32(destination,
                static_cast<std::int32_t>(quantizeSigned(sample, 32)),
                bigEndian);
        return true;
    } else if constexpr (Format == AsioSampleFormat::int32_msb16 ||
                         Format == AsioSampleFormat::int32_lsb16) {
        write32(destination, static_cast<std::int32_t>(quantizeSigned(sample, 16)), bigEndian);
        return true;
    } else if constexpr (Format == AsioSampleFormat::int32_msb18 ||
                         Format == AsioSampleFormat::int32_lsb18) {
        write32(destination, static_cast<std::int32_t>(quantizeSigned(sample, 18)), bigEndian);
        return true;
    } else if constexpr (Format == AsioSampleFormat::int32_msb20 ||
                         Format == AsioSampleFormat::int32_lsb20) {
        write32(destination, static_cast<std::int32_t>(quantizeSigned(sample, 20)), bigEndian);
        return true;
    } else if constexpr (Format == AsioSampleFormat::int32_msb24 ||
                         Format == AsioSampleFormat::int32_lsb24) {
        write32(destination, static_cast<std::int32_t>(quantizeSigned(sample, 24)), bigEndian);
        return true;
    } else if constexpr (Format == AsioSampleFormat::float32_msb ||
                         Format == AsioSampleFormat::float32_lsb) {
        const float clean = finiteOrZero(sample);
        std::uint32_t word{};
        std::memcpy(&word, &clean, sizeof(word));
        auto* bytes = static_cast<std::uint8_t*>(destination);
        if constexpr (bigEndian) {
            bytes[0] = static_cast<std::uint8_t>(word >> 24);
            bytes[1] = static_cast<std::uint8_t>(word >> 16);
            bytes[2] = static_cast<std::uint8_t>(word >> 8);
            bytes[3] = static_cast<std::uint8_t>(word);
        } else {
            bytes[0] = static_cast<std::uint8_t>(word);
            bytes[1] = static_cast<std::uint8_t>(word >> 8);
            bytes[2] = static_cast<std::uint8_t>(word >> 16);
            bytes[3] = static_cast<std::uint8_t>(word >> 24);
        }
        return true;
    } else if constexpr (Format == AsioSampleFormat::float64_msb ||
                         Format == AsioSampleFormat::float64_lsb) {
        write64Float(destination,
                     static_cast<double>(finiteOrZero(sample)),
                     bigEndian);
        return true;
    } else {
        return false;
    }
}

template <AsioSampleFormat Format>
void encodeSampleBufferTyped(const float* samples,
                             std::uint8_t* destination,
                             std::size_t frames,
                             std::uint32_t bytesPerSample) noexcept {
    for (std::size_t frame = 0; frame < frames; ++frame) {
        (void)encodeSampleTyped<Format>(samples[frame],
                                        destination + frame * bytesPerSample);
    }
}

void encodeSampleBuffer(const float* samples,
                        std::uint8_t* destination,
                        std::size_t frames,
                        std::uint32_t bytesPerSample,
                        AsioSampleFormat format) noexcept {
    switch (format) {
#define AXE_ENCODE_SAMPLE_BUFFER_CASE(formatName) \
    case AsioSampleFormat::formatName: \
        encodeSampleBufferTyped<AsioSampleFormat::formatName>( \
            samples, destination, frames, bytesPerSample); \
        return
        AXE_ENCODE_SAMPLE_BUFFER_CASE(int16_msb);
        AXE_ENCODE_SAMPLE_BUFFER_CASE(int24_msb);
        AXE_ENCODE_SAMPLE_BUFFER_CASE(int32_msb);
        AXE_ENCODE_SAMPLE_BUFFER_CASE(float32_msb);
        AXE_ENCODE_SAMPLE_BUFFER_CASE(float64_msb);
        AXE_ENCODE_SAMPLE_BUFFER_CASE(int32_msb16);
        AXE_ENCODE_SAMPLE_BUFFER_CASE(int32_msb18);
        AXE_ENCODE_SAMPLE_BUFFER_CASE(int32_msb20);
        AXE_ENCODE_SAMPLE_BUFFER_CASE(int32_msb24);
        AXE_ENCODE_SAMPLE_BUFFER_CASE(int16_lsb);
        AXE_ENCODE_SAMPLE_BUFFER_CASE(int24_lsb);
        AXE_ENCODE_SAMPLE_BUFFER_CASE(int32_lsb);
        AXE_ENCODE_SAMPLE_BUFFER_CASE(float32_lsb);
        AXE_ENCODE_SAMPLE_BUFFER_CASE(float64_lsb);
        AXE_ENCODE_SAMPLE_BUFFER_CASE(int32_lsb16);
        AXE_ENCODE_SAMPLE_BUFFER_CASE(int32_lsb18);
        AXE_ENCODE_SAMPLE_BUFFER_CASE(int32_lsb20);
        AXE_ENCODE_SAMPLE_BUFFER_CASE(int32_lsb24);
#undef AXE_ENCODE_SAMPLE_BUFFER_CASE
        default:
            std::memset(destination, 0, frames * bytesPerSample);
            return;
    }
}

AsioSampleFormat fromSampleType(ASIOSampleType format) noexcept {
    return static_cast<AsioSampleFormat>(format);
}

std::string boundedName(const char* name, std::size_t capacity) {
    const auto* end = static_cast<const char*>(std::memchr(name, '\0', capacity));
    return std::string(name, end ? end : name + capacity);
}

}  // namespace

bool encodeAsioSample(float sample, AsioSampleFormat format, void* destination) noexcept {
    switch (format) {
        case AsioSampleFormat::int16_msb:
            return encodeSampleTyped<AsioSampleFormat::int16_msb>(sample, destination);
        case AsioSampleFormat::int24_msb:
            return encodeSampleTyped<AsioSampleFormat::int24_msb>(sample, destination);
        case AsioSampleFormat::int32_msb:
            return encodeSampleTyped<AsioSampleFormat::int32_msb>(sample, destination);
        case AsioSampleFormat::float32_msb:
            return encodeSampleTyped<AsioSampleFormat::float32_msb>(sample, destination);
        case AsioSampleFormat::float64_msb:
            return encodeSampleTyped<AsioSampleFormat::float64_msb>(sample, destination);
        case AsioSampleFormat::int32_msb16:
            return encodeSampleTyped<AsioSampleFormat::int32_msb16>(sample, destination);
        case AsioSampleFormat::int32_msb18:
            return encodeSampleTyped<AsioSampleFormat::int32_msb18>(sample, destination);
        case AsioSampleFormat::int32_msb20:
            return encodeSampleTyped<AsioSampleFormat::int32_msb20>(sample, destination);
        case AsioSampleFormat::int32_msb24:
            return encodeSampleTyped<AsioSampleFormat::int32_msb24>(sample, destination);
        case AsioSampleFormat::int16_lsb:
            return encodeSampleTyped<AsioSampleFormat::int16_lsb>(sample, destination);
        case AsioSampleFormat::int24_lsb:
            return encodeSampleTyped<AsioSampleFormat::int24_lsb>(sample, destination);
        case AsioSampleFormat::int32_lsb:
            return encodeSampleTyped<AsioSampleFormat::int32_lsb>(sample, destination);
        case AsioSampleFormat::float32_lsb:
            return encodeSampleTyped<AsioSampleFormat::float32_lsb>(sample, destination);
        case AsioSampleFormat::float64_lsb:
            return encodeSampleTyped<AsioSampleFormat::float64_lsb>(sample, destination);
        case AsioSampleFormat::int32_lsb16:
            return encodeSampleTyped<AsioSampleFormat::int32_lsb16>(sample, destination);
        case AsioSampleFormat::int32_lsb18:
            return encodeSampleTyped<AsioSampleFormat::int32_lsb18>(sample, destination);
        case AsioSampleFormat::int32_lsb20:
            return encodeSampleTyped<AsioSampleFormat::int32_lsb20>(sample, destination);
        case AsioSampleFormat::int32_lsb24:
            return encodeSampleTyped<AsioSampleFormat::int32_lsb24>(sample, destination);
    }

    return false;
}

bool decodeAsioSample(const void* source,
                      AsioSampleFormat format,
                      float& sample) noexcept {
    switch (format) {
#define AXE_DECODE_SAMPLE_CASE(formatName) \
    case AsioSampleFormat::formatName: \
        return decodeSampleTyped<AsioSampleFormat::formatName>(source, sample)
        AXE_DECODE_SAMPLE_CASE(int16_msb);
        AXE_DECODE_SAMPLE_CASE(int24_msb);
        AXE_DECODE_SAMPLE_CASE(int32_msb);
        AXE_DECODE_SAMPLE_CASE(float32_msb);
        AXE_DECODE_SAMPLE_CASE(float64_msb);
        AXE_DECODE_SAMPLE_CASE(int32_msb16);
        AXE_DECODE_SAMPLE_CASE(int32_msb18);
        AXE_DECODE_SAMPLE_CASE(int32_msb20);
        AXE_DECODE_SAMPLE_CASE(int32_msb24);
        AXE_DECODE_SAMPLE_CASE(int16_lsb);
        AXE_DECODE_SAMPLE_CASE(int24_lsb);
        AXE_DECODE_SAMPLE_CASE(int32_lsb);
        AXE_DECODE_SAMPLE_CASE(float32_lsb);
        AXE_DECODE_SAMPLE_CASE(float64_lsb);
        AXE_DECODE_SAMPLE_CASE(int32_lsb16);
        AXE_DECODE_SAMPLE_CASE(int32_lsb18);
        AXE_DECODE_SAMPLE_CASE(int32_lsb20);
        AXE_DECODE_SAMPLE_CASE(int32_lsb24);
#undef AXE_DECODE_SAMPLE_CASE
    }
    sample = 0.0f;
    return false;
}

std::uint32_t asioSampleFormatBytes(AsioSampleFormat format) noexcept {
    switch (format) {
        case AsioSampleFormat::int16_msb:
        case AsioSampleFormat::int16_lsb:
            return 2;
        case AsioSampleFormat::int24_msb:
        case AsioSampleFormat::int24_lsb:
            return 3;
        case AsioSampleFormat::int32_msb:
        case AsioSampleFormat::float32_msb:
        case AsioSampleFormat::int32_msb16:
        case AsioSampleFormat::int32_msb18:
        case AsioSampleFormat::int32_msb20:
        case AsioSampleFormat::int32_msb24:
        case AsioSampleFormat::int32_lsb:
        case AsioSampleFormat::float32_lsb:
        case AsioSampleFormat::int32_lsb16:
        case AsioSampleFormat::int32_lsb18:
        case AsioSampleFormat::int32_lsb20:
        case AsioSampleFormat::int32_lsb24:
            return 4;
        case AsioSampleFormat::float64_msb:
        case AsioSampleFormat::float64_lsb:
            return 8;
    }
    return 0;
}

const char* asioSampleFormatName(AsioSampleFormat format) noexcept {
    switch (format) {
        case AsioSampleFormat::int16_msb: return "Int16MSB";
        case AsioSampleFormat::int24_msb: return "Int24MSB";
        case AsioSampleFormat::int32_msb: return "Int32MSB";
        case AsioSampleFormat::float32_msb: return "Float32MSB";
        case AsioSampleFormat::float64_msb: return "Float64MSB";
        case AsioSampleFormat::int32_msb16: return "Int32MSB16";
        case AsioSampleFormat::int32_msb18: return "Int32MSB18";
        case AsioSampleFormat::int32_msb20: return "Int32MSB20";
        case AsioSampleFormat::int32_msb24: return "Int32MSB24";
        case AsioSampleFormat::int16_lsb: return "Int16LSB";
        case AsioSampleFormat::int24_lsb: return "Int24LSB";
        case AsioSampleFormat::int32_lsb: return "Int32LSB";
        case AsioSampleFormat::float32_lsb: return "Float32LSB";
        case AsioSampleFormat::float64_lsb: return "Float64LSB";
        case AsioSampleFormat::int32_lsb16: return "Int32LSB16";
        case AsioSampleFormat::int32_lsb18: return "Int32LSB18";
        case AsioSampleFormat::int32_lsb20: return "Int32LSB20";
        case AsioSampleFormat::int32_lsb24: return "Int32LSB24";
    }
    return "Unknown";
}

struct AsioHost::Impl final {
    struct Gate final {
        std::atomic<Impl*> owner{nullptr};
        std::atomic<std::uint32_t> inFlight{0};
        std::atomic<bool> claimed{false};
    };

    static constexpr std::size_t kCallbackSlotCount = 64;

    static std::array<Gate, kCallbackSlotCount>& gates() noexcept {
        static std::array<Gate, kCallbackSlotCount> instances{};
        return instances;
    }

    template <std::size_t Slot>
    static ASIOCallbacks makeCallbackTable() noexcept {
        return ASIOCallbacks{
            &Impl::bufferSwitchThunk<Slot>,
            &Impl::sampleRateChangedThunk<Slot>,
            &Impl::asioMessageThunk<Slot>,
            &Impl::bufferSwitchTimeInfoThunk<Slot>,
        };
    }

    template <std::size_t... Slots>
    static std::array<ASIOCallbacks, sizeof...(Slots)> makeCallbackTables(
        std::index_sequence<Slots...>) noexcept {
        return {makeCallbackTable<Slots>()...};
    }

    static std::array<ASIOCallbacks, kCallbackSlotCount>& callbackTables() noexcept {
        static auto tables = makeCallbackTables(
            std::make_index_sequence<kCallbackSlotCount>{});
        return tables;
    }

    template <std::size_t Slot>
    static void bufferSwitchThunk(long index, ASIOBool /*directProcess*/) noexcept {
        auto& shared = gates()[Slot];
        shared.inFlight.fetch_add(1, std::memory_order_seq_cst);
        Impl* owner = shared.owner.load(std::memory_order_seq_cst);
        if (owner != nullptr) {
            owner->dispatchBuffer(index);
        }
        shared.inFlight.fetch_sub(1, std::memory_order_seq_cst);
    }

    template <std::size_t Slot>
    static void sampleRateChangedThunk(ASIOSampleRate /*sampleRate*/) noexcept {
        withOwner<Slot>([](Impl& owner) {
            owner.notifications.fetch_or(asio_notify_sample_rate_changed,
                                         std::memory_order_relaxed);
        });
    }

    template <std::size_t Slot>
    static ASIOTime* bufferSwitchTimeInfoThunk(ASIOTime* timeInfo,
                                               long index,
                                               ASIOBool /*directProcess*/) noexcept {
        bufferSwitchThunk<Slot>(index, ASIOTrue);
        return timeInfo;
    }

    template <std::size_t Slot>
    static long asioMessageThunk(long selector,
                                 long value,
                                 void* /*message*/,
                                 double* /*opt*/) noexcept {
        long result = 0;
        withOwner<Slot>([&](Impl& owner) {
            switch (selector) {
                case kAsioSelectorSupported:
                    result = (value == kAsioEngineVersion || value == kAsioResetRequest ||
                              value == kAsioBufferSizeChange || value == kAsioResyncRequest ||
                              value == kAsioLatenciesChanged || value == kAsioSupportsTimeInfo)
                                 ? 1
                                 : 0;
                    break;
                case kAsioEngineVersion:
                    result = 2;
                    break;
                case kAsioResetRequest:
                    owner.notifications.fetch_or(asio_notify_reset, std::memory_order_relaxed);
                    result = 1;
                    break;
                case kAsioBufferSizeChange:
                    owner.notifications.fetch_or(asio_notify_buffer_size_changed,
                                                 std::memory_order_relaxed);
                    result = 1;
                    break;
                case kAsioResyncRequest:
                    owner.notifications.fetch_or(asio_notify_resync, std::memory_order_relaxed);
                    result = 1;
                    break;
                case kAsioLatenciesChanged:
                    owner.notifications.fetch_or(asio_notify_latencies_changed,
                                                 std::memory_order_relaxed);
                    result = 1;
                    break;
                case kAsioSupportsTimeInfo:
                    result = 1;
                    break;
                case kAsioOverload:
                    owner.notifications.fetch_or(asio_notify_overload,
                                                 std::memory_order_relaxed);
                    result = 1;
                    break;
                default:
                    result = 0;
                    break;
            }
        });
        return result;
    }

    template <std::size_t Slot, typename Callback>
    static void withOwner(Callback&& callback) noexcept {
        auto& shared = gates()[Slot];
        shared.inFlight.fetch_add(1, std::memory_order_seq_cst);
        Impl* owner = shared.owner.load(std::memory_order_seq_cst);
        if (owner != nullptr) {
            callback(*owner);
        }
        shared.inFlight.fetch_sub(1, std::memory_order_seq_cst);
    }

    AsioHostStatus failure(AsioHostError code,
                           std::string message,
                           long nativeCode = 0) const {
        return AsioHostStatus{code, nativeCode, std::move(message)};
    }

    AsioHostStatus checkThread() const {
        if (ownerThreadId != 0 && ownerThreadId != GetCurrentThreadId()) {
            return failure(AsioHostError::wrong_thread,
                           "ASIO lifecycle calls must stay on the thread that opened the driver.");
        }
        return {};
    }

    AsioHostStatus driverFailure(const char* operation, ASIOError result) {
        char driverMessage[124]{};
        if (driver != nullptr) {
            driver->getErrorMessage(driverMessage);
        }
        const std::string detail = boundedName(driverMessage, sizeof(driverMessage));
        std::string text(operation);
        text += " failed (ASIO error ";
        text += std::to_string(result);
        text += ")";
        if (!detail.empty()) {
            text += ": ";
            text += detail;
        }
        return failure(AsioHostError::driver_call, std::move(text), result);
    }

    AsioHostStatus activationFailure(HRESULT result) const {
        std::string text = "Could not activate the registered Fractal FAS ASIO driver (HRESULT ";
        text += std::to_string(static_cast<long>(result));
        text += "). Verify the 64-bit FAS ASIO Driver is installed and registered.";
        return failure(AsioHostError::driver_activation,
                       std::move(text),
                       static_cast<long>(result));
    }

    AsioHostStatus open(AsioRenderCallback callback,
                        void* context,
                        void* requestedWindow,
                        const AsioInputTap* inputTap) {
        if (driver != nullptr || comUsable) {
            return failure(AsioHostError::invalid_state, "The ASIO driver is already open.");
        }

        ownerThreadId = GetCurrentThreadId();
        renderCallback = callback;
        renderContext = context;
        inputTapEnabled = inputTap != nullptr && inputTap->callback != nullptr;
        inputTapCallback = inputTapEnabled ? inputTap->callback : nullptr;
        inputTapContext = inputTapEnabled ? inputTap->callbackContext : nullptr;
        selectedInputFirstChannel = inputTapEnabled ? inputTap->firstChannel : -1;
        if (!reserveCallbackSlot()) {
            ownerThreadId = 0;
            return failure(AsioHostError::callback_already_active,
                           "This process has used all available ASIO callback lifetime slots.");
        }
        publishCallbackOwner();
        HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (SUCCEEDED(hr)) {
            comInitialized = true;
            comUsable = true;
        } else if (hr == RPC_E_CHANGED_MODE) {
            // COM is already usable in this thread's existing apartment; do not
            // balance another apartment's initialization with CoUninitialize.
            comUsable = true;
        } else {
            releaseCallbackSlot(true);
            ownerThreadId = 0;
            return failure(AsioHostError::com_initialization,
                           "COM initialization failed for ASIO driver activation.",
                           static_cast<long>(hr));
        }

        HRESULT activation = E_FAIL;
#if defined(AXE_ASIO_TESTING)
        if (const auto testFactory =
                asio_host_internal::gDriverFactoryForTests.load(std::memory_order_acquire)) {
            activation = static_cast<HRESULT>(
                testFactory(reinterpret_cast<void**>(&driver)));
        } else {
#endif
            activation = CoCreateInstance(
                kFractalAxeFxIiiAsioClsid,
                nullptr,
                CLSCTX_INPROC_SERVER,
                kFractalAxeFxIiiAsioClsid,
                reinterpret_cast<void**>(&driver));
#if defined(AXE_ASIO_TESTING)
        }
#endif
        if (FAILED(activation)) {
            const auto status = activationFailure(activation);
            (void)close();
            return status;
        }

        HWND window = static_cast<HWND>(requestedWindow);
        if (window == nullptr) {
            window = GetConsoleWindow();
        }
        systemWindowHandle = window;
        if (window == nullptr) {
            const auto status = failure(AsioHostError::driver_initialization,
                                        "ASIO requires a valid host HWND; pass the application window handle or run from a console.");
            (void)close();
            return status;
        }

        if (driver->init(window) == ASIOFalse) {
            char driverMessage[124]{};
            driver->getErrorMessage(driverMessage);
            std::string text = "The Fractal ASIO driver refused initialization.";
            const std::string detail = boundedName(driverMessage, sizeof(driverMessage));
            if (!detail.empty()) {
                text += " ";
                text += detail;
            }
            const auto status = failure(AsioHostError::driver_initialization, std::move(text));
            (void)close();
            return status;
        }
        driverInitialized = true;

        char name[32]{};
        driver->getDriverName(name);
        capabilities.driverName = boundedName(name, sizeof(name));
        capabilities.driverVersion = driver->getDriverVersion();

        ASIOError result = driver->getChannels(&capabilities.inputChannelCount,
                                               &capabilities.outputChannelCount);
        if (result != ASE_OK) {
            const auto status = driverFailure("IASIO::getChannels", result);
            (void)close();
            return status;
        }
        if (capabilities.outputChannelCount <= AsioHost::kOutputChannels.back()) {
            const auto status = failure(
                AsioHostError::missing_channels,
                "The Fractal ASIO driver exposes fewer than eight output channels; Axe-Fx USB 7/8 are unavailable.");
            (void)close();
            return status;
        }
        if (inputTapEnabled &&
            (capabilities.inputChannelCount < 2 || selectedInputFirstChannel < 0 ||
             selectedInputFirstChannel > capabilities.inputChannelCount - 2)) {
            const auto status = failure(
                AsioHostError::missing_channels,
                "The requested ASIO input pair is outside the driver's reported input channels.");
            (void)close();
            return status;
        }

        result = driver->getBufferSize(&capabilities.minimumBufferFrames,
                                       &capabilities.maximumBufferFrames,
                                       &capabilities.preferredBufferFrames,
                                       &capabilities.bufferGranularity);
        if (result != ASE_OK) {
            const auto status = driverFailure("IASIO::getBufferSize", result);
            (void)close();
            return status;
        }
        if (capabilities.minimumBufferFrames <= 0 ||
            capabilities.maximumBufferFrames < capabilities.minimumBufferFrames ||
            capabilities.preferredBufferFrames <= 0) {
            const auto status = failure(AsioHostError::driver_call,
                                        "The ASIO driver reported invalid buffer-size limits.");
            (void)close();
            return status;
        }

        ASIOSampleRate currentRate = 0.0;
        result = driver->getSampleRate(&currentRate);
        if (result != ASE_OK) {
            const auto status = driverFailure("IASIO::getSampleRate", result);
            (void)close();
            return status;
        }
        capabilities.currentSampleRate = currentRate;
        originalSampleRate_ = currentRate;
        capabilities.supports48kHz = driver->canSampleRate(kTargetRate) == ASE_OK;

        for (std::size_t i = 0; i < AsioHost::kOutputChannels.size(); ++i) {
            ASIOChannelInfo info{};
            info.channel = AsioHost::kOutputChannels[i];
            info.isInput = ASIOFalse;
            result = driver->getChannelInfo(&info);
            if (result != ASE_OK) {
                const auto status = driverFailure("IASIO::getChannelInfo(output)", result);
                (void)close();
                return status;
            }
            auto& selected = capabilities.selectedOutputs[i];
            selected.channelNumber = info.channel;
            selected.name = boundedName(info.name, sizeof(info.name));
            selected.format = fromSampleType(info.type);
            selected.bytesPerSample = asioSampleFormatBytes(selected.format);
            selected.formatSupported = selected.bytesPerSample != 0;
        }

        if (inputTapEnabled) {
            capabilities.inputTapEnabled = true;
            capabilities.selectedInputFirstChannel = selectedInputFirstChannel;
            for (std::size_t i = 0; i < capabilities.selectedInputs.size(); ++i) {
                ASIOChannelInfo info{};
                info.channel = selectedInputFirstChannel + static_cast<long>(i);
                info.isInput = ASIOTrue;
                result = driver->getChannelInfo(&info);
                if (result != ASE_OK) {
                    const auto status = driverFailure("IASIO::getChannelInfo(input)", result);
                    (void)close();
                    return status;
                }
                auto& selected = capabilities.selectedInputs[i];
                selected.channelNumber = info.channel;
                selected.name = boundedName(info.name, sizeof(info.name));
                selected.format = fromSampleType(info.type);
                selected.bytesPerSample = asioSampleFormatBytes(selected.format);
                selected.formatSupported = selected.bytesPerSample != 0;
            }
            if (!capabilities.selectedInputs[0].formatSupported ||
                !capabilities.selectedInputs[1].formatSupported) {
                const auto status = failure(
                    AsioHostError::unsupported_format,
                    "The selected ASIO input pair uses an unsupported sample format.");
                (void)close();
                return status;
            }
        }

        if (!capabilities.selectedOutputs[0].formatSupported ||
            !capabilities.selectedOutputs[1].formatSupported) {
            const auto status = failure(AsioHostError::unsupported_format,
                                        "The selected Axe-Fx USB 7/8 channels use an unsupported ASIO sample format.");
            (void)close();
            return status;
        }

        return {};
    }

    bool bufferSizeValid(long frames) const noexcept {
        if (frames < capabilities.minimumBufferFrames ||
            frames > capabilities.maximumBufferFrames) {
            return false;
        }
        if (capabilities.bufferGranularity == -1) {
            const auto value = static_cast<unsigned long>(frames);
            return value != 0 && (value & (value - 1)) == 0;
        }
        if (capabilities.bufferGranularity == 0) {
            return frames == capabilities.preferredBufferFrames;
        }
        if (capabilities.bufferGranularity > 0) {
            return ((frames - capabilities.minimumBufferFrames) %
                    capabilities.bufferGranularity) == 0;
        }
        return false;
    }

    bool chooseBufferSize(long requested, long& frames) const noexcept {
        const long candidate = requested == 0 ? capabilities.preferredBufferFrames : requested;
        if (bufferSizeValid(candidate)) {
            frames = candidate;
            return true;
        }
        return false;
    }

    AsioHostStatus start(long requestedFrames) {
        if (driver == nullptr || !driverInitialized) {
            return failure(AsioHostError::invalid_state, "Open the ASIO driver before starting it.");
        }
        if (started_ || buffersCreated) {
            return failure(AsioHostError::invalid_state, "The ASIO driver is already started or has active buffers.");
        }
        if (!capabilities.supports48kHz) {
            return failure(AsioHostError::unsupported_sample_rate,
                           "The Fractal driver does not report support for the required 48 kHz rate.");
        }

        if (!callbackSlotClaimed) {
            return failure(AsioHostError::callback_already_active,
                           "The ASIO host callback lifetime slot was not reserved during open().");
        }
        publishCallbackOwner();

        long frames = 0;
        if (!chooseBufferSize(requestedFrames, frames)) {
            unpublishCallbackOwnerAndDrain();
            return failure(AsioHostError::invalid_state,
                           "Requested ASIO buffer size is outside the driver limits or violates its granularity.");
        }
        capabilities.outputReadySupported = false;
        outputReadyEnabled.store(false, std::memory_order_release);

        if (std::abs(capabilities.currentSampleRate - kTargetRate) > kRateTolerance) {
            ASIOError rateResult = driver->setSampleRate(kTargetRate);
            if (rateResult != ASE_OK) {
                const auto status = driverFailure("IASIO::setSampleRate(48000)", rateResult);
                unpublishCallbackOwnerAndDrain();
                return status;
            }
            sampleRateChangedByUs = true;
            capabilities.currentSampleRate = kTargetRate;
        }

        try {
            scratchLeft.resize(static_cast<std::size_t>(frames));
            scratchRight.resize(static_cast<std::size_t>(frames));
            if (inputTapEnabled) {
                inputScratchLeft.resize(static_cast<std::size_t>(frames));
                inputScratchRight.resize(static_cast<std::size_t>(frames));
            }
        } catch (const std::bad_alloc&) {
            unpublishCallbackOwnerAndDrain();
            restoreSampleRateIfNeeded();
            return failure(AsioHostError::allocation_failure,
                           "Not enough memory to allocate fixed ASIO input/output scratch buffers.");
        }

        selectedBufferFrames = frames;
        capabilities.activeBufferFrames = frames;
        outputDescriptorOffset = inputTapEnabled ? 2 : 0;
        activeBufferInfoCount = inputTapEnabled ? 4 : 2;
        bufferInfos = {};
        if (inputTapEnabled) {
            bufferInfos[0].isInput = ASIOTrue;
            bufferInfos[0].channelNum = selectedInputFirstChannel;
            bufferInfos[1].isInput = ASIOTrue;
            bufferInfos[1].channelNum = selectedInputFirstChannel + 1;
        }
        bufferInfos[outputDescriptorOffset].isInput = ASIOFalse;
        bufferInfos[outputDescriptorOffset].channelNum = AsioHost::kOutputChannels[0];
        bufferInfos[outputDescriptorOffset + 1].isInput = ASIOFalse;
        bufferInfos[outputDescriptorOffset + 1].channelNum = AsioHost::kOutputChannels[1];

        const ASIOError createResult = driver->createBuffers(
            bufferInfos.data(),
            activeBufferInfoCount,
            frames,
            &callbackTables()[callbackSlot]);
        if (createResult != ASE_OK) {
            const auto status = driverFailure("IASIO::createBuffers", createResult);
            unpublishCallbackOwnerAndDrain();
            if (driver->disposeBuffers() != ASE_OK) {
                callbackLifetimeSafe = false;
            }
            buffersCreated = false;
            restoreSampleRateIfNeeded();
            return status;
        }
        buffersCreated = true;
        for (std::size_t channel = 0;
             channel < static_cast<std::size_t>(activeBufferInfoCount);
             ++channel) {
            for (std::size_t half = 0; half < 2; ++half) {
                if (bufferInfos[channel].buffers[half] == nullptr) {
                    const auto status = failure(AsioHostError::driver_call,
                                                "The ASIO driver created a null output buffer.");
                    unpublishCallbackOwnerAndDrain();
                    if (driver->disposeBuffers() != ASE_OK) {
                        callbackLifetimeSafe = false;
                    }
                    buffersCreated = false;
                    restoreSampleRateIfNeeded();
                    return status;
                }
            }
        }

        // ASIO sends buffer 1 to the device after start; seed that half before
        // starting, as required by the SDK's double-buffer streaming contract.
        renderOutputBuffer(1);
        // The ASIO SDK defines outputReady as an optional notification that
        // processed output is ready for the device. Probe it only after buffers
        // exist and the initial output half contains complete samples; if a
        // driver declines or errors, ordinary double-buffer streaming remains
        // available and start must still proceed.
        const ASIOError outputReadyResult = driver->outputReady();
        capabilities.outputReadySupported = outputReadyResult == ASE_OK;
        outputReadyEnabled.store(capabilities.outputReadySupported,
                                 std::memory_order_release);

        // Drivers that accept outputReady may report a different output
        // latency after the first notification, so read latency metadata only
        // after negotiation and the priming notification have completed.
        long inputLatency = -1;
        long outputLatency = -1;
        if (driver->getLatencies(&inputLatency, &outputLatency) == ASE_OK) {
            capabilities.inputLatencyFrames = inputLatency;
            capabilities.outputLatencyFrames = outputLatency;
        }

        const ASIOError startResult = driver->start();
        if (startResult != ASE_OK) {
            const auto status = driverFailure("IASIO::start", startResult);
            if (driver->stop() != ASE_OK) {
                callbackLifetimeSafe = false;
            }
            unpublishCallbackOwnerAndDrain();
            if (driver->disposeBuffers() != ASE_OK) {
                callbackLifetimeSafe = false;
            }
            outputReadyEnabled.store(false, std::memory_order_release);
            buffersCreated = false;
            restoreSampleRateIfNeeded();
            return status;
        }
        started_ = true;
        return {};
    }

    void captureInputBuffer(long bufferIndex) noexcept {
        if (!inputTapEnabled || inputTapCallback == nullptr || !buffersCreated ||
            (bufferIndex != 0 && bufferIndex != 1) || selectedBufferFrames <= 0) {
            return;
        }

        const auto frames = static_cast<std::size_t>(selectedBufferFrames);
        const auto* left = static_cast<const std::uint8_t*>(
            bufferInfos[0].buffers[bufferIndex]);
        const auto* right = static_cast<const std::uint8_t*>(
            bufferInfos[1].buffers[bufferIndex]);
        if (left == nullptr || right == nullptr || inputScratchLeft.size() < frames ||
            inputScratchRight.size() < frames) {
            return;
        }

        decodeSampleBuffer(left,
                           inputScratchLeft.data(),
                           frames,
                           capabilities.selectedInputs[0].bytesPerSample,
                           capabilities.selectedInputs[0].format);
        decodeSampleBuffer(right,
                           inputScratchRight.data(),
                           frames,
                           capabilities.selectedInputs[1].bytesPerSample,
                           capabilities.selectedInputs[1].format);
        inputTapCallback(inputTapContext,
                         inputScratchLeft.data(),
                         inputScratchRight.data(),
                         static_cast<std::uint32_t>(selectedBufferFrames));
    }

    void renderOutputBuffer(long bufferIndex) noexcept {
        if (!buffersCreated || (bufferIndex != 0 && bufferIndex != 1) ||
            selectedBufferFrames <= 0) {
            return;
        }

        void* leftBuffer = bufferInfos[outputDescriptorOffset].buffers[bufferIndex];
        void* rightBuffer = bufferInfos[outputDescriptorOffset + 1].buffers[bufferIndex];
        const auto frames = static_cast<std::size_t>(selectedBufferFrames);
        const auto leftBytes = capabilities.selectedOutputs[0].bytesPerSample;
        const auto rightBytes = capabilities.selectedOutputs[1].bytesPerSample;
        if (leftBuffer != nullptr) {
            std::memset(leftBuffer, 0, frames * leftBytes);
        }
        if (rightBuffer != nullptr) {
            std::memset(rightBuffer, 0, frames * rightBytes);
        }

        if (renderCallback == nullptr || leftBuffer == nullptr || rightBuffer == nullptr ||
            scratchLeft.size() < frames || scratchRight.size() < frames) {
            return;
        }

        std::fill_n(scratchLeft.data(), frames, 0.0f);
        std::fill_n(scratchRight.data(), frames, 0.0f);
        renderCallback(renderContext,
                       scratchLeft.data(),
                       scratchRight.data(),
                       static_cast<std::uint32_t>(selectedBufferFrames));

        auto* left = static_cast<std::uint8_t*>(leftBuffer);
        auto* right = static_cast<std::uint8_t*>(rightBuffer);
        encodeSampleBuffer(scratchLeft.data(),
                           left,
                           frames,
                           leftBytes,
                           capabilities.selectedOutputs[0].format);
        encodeSampleBuffer(scratchRight.data(),
                           right,
                           frames,
                           rightBytes,
                           capabilities.selectedOutputs[1].format);
    }

    void notifyOutputReady() noexcept {
        if (!outputReadyEnabled.load(std::memory_order_acquire) || driver == nullptr) {
            return;
        }
        // outputReady is an optimization. If the driver stops accepting it,
        // disable further notifications without interrupting audio streaming.
        if (driver->outputReady() != ASE_OK) {
            outputReadyEnabled.store(false, std::memory_order_release);
        }
    }

    void dispatchBuffer(long index) noexcept {
        callbackCounter.fetch_add(1, std::memory_order_relaxed);
        if (!buffersCreated || (index != 0 && index != 1)) {
            return;
        }
        captureInputBuffer(index);
        renderOutputBuffer(index);
        notifyOutputReady();
    }

    void unpublishCallbackOwnerAndDrain() noexcept {
        if (callbackSlotClaimed) {
            auto& shared = gates()[callbackSlot];
            shared.owner.exchange(nullptr, std::memory_order_seq_cst);
            while (shared.inFlight.load(std::memory_order_seq_cst) != 0) {
                Sleep(1);
            }
        }
    }

    bool reserveCallbackSlot() noexcept {
        auto& allGates = gates();
        for (std::size_t i = 0; i < allGates.size(); ++i) {
            bool expected = false;
            if (allGates[i].claimed.compare_exchange_strong(expected,
                                                            true,
                                                            std::memory_order_acq_rel)) {
                callbackSlot = i;
                callbackSlotClaimed = true;
                callbackLifetimeSafe = true;
                return true;
            }
        }
        return false;
    }

    void publishCallbackOwner() noexcept {
        if (callbackSlotClaimed) {
            gates()[callbackSlot].owner.store(this, std::memory_order_seq_cst);
        }
    }

    void releaseCallbackSlot(bool permitReuse) noexcept {
        if (!callbackSlotClaimed) {
            return;
        }
        unpublishCallbackOwnerAndDrain();
        // ASIO carries no callback context, so a slot is recycled only after a
        // successful stop/dispose/release sequence and a complete callback drain.
        // When any shutdown call fails, leave the static slot retired; even a
        // late callback from that driver can then never target a future host.
        if (permitReuse && callbackLifetimeSafe) {
            gates()[callbackSlot].claimed.store(false, std::memory_order_release);
        }
        callbackSlotClaimed = false;
    }

    void restoreSampleRateIfNeeded() noexcept {
        if (sampleRateChangedByUs && driver != nullptr && driverInitialized &&
            std::isfinite(originalSampleRate_) && originalSampleRate_ > 0.0) {
            (void)driver->setSampleRate(originalSampleRate_);
            capabilities.currentSampleRate = originalSampleRate_;
        }
        sampleRateChangedByUs = false;
    }

    AsioHostStatus stop() {
        const auto threadStatus = checkThread();
        if (!threadStatus) {
            return threadStatus;
        }
        if (driver == nullptr || !started_) {
            return {};
        }

        const ASIOError result = driver->stop();
        // stop() is required to cease device streaming; nulling the registry and
        // draining callbacks before disposal also protects against an in-flight
        // driver callback that began just before stop returned.
        unpublishCallbackOwnerAndDrain();
        started_ = false;
        if (result != ASE_OK) {
            callbackLifetimeSafe = false;
            return driverFailure("IASIO::stop", result);
        }
        return {};
    }

    AsioHostStatus close() {
        const auto threadStatus = checkThread();
        if (!threadStatus) {
            return threadStatus;
        }

        AsioHostStatus firstFailure{};
        bool shutdownSucceeded = callbackLifetimeSafe;
        if (started_) {
            firstFailure = stop();
            if (!firstFailure) {
                shutdownSucceeded = false;
            }
        }
        if (driver != nullptr && buffersCreated) {
            const ASIOError result = driver->disposeBuffers();
            buffersCreated = false;
            capabilities.activeBufferFrames = 0;
            if (result != ASE_OK && firstFailure) {
                firstFailure = driverFailure("IASIO::disposeBuffers", result);
            }
            if (result != ASE_OK) {
                shutdownSucceeded = false;
            }
        }
        restoreSampleRateIfNeeded();
        if (driver != nullptr && driverInitialized) {
            driverInitialized = false;
        }
        if (driver != nullptr) {
            driver->Release();
            driver = nullptr;
        }
        unpublishCallbackOwnerAndDrain();
        releaseCallbackSlot(shutdownSucceeded);
        systemWindowHandle = nullptr;
        if (comInitialized) {
            CoUninitialize();
            comInitialized = false;
        }
        comUsable = false;
        ownerThreadId = 0;
        renderCallback = nullptr;
        renderContext = nullptr;
        scratchLeft.clear();
        scratchRight.clear();
        inputScratchLeft.clear();
        inputScratchRight.clear();
        selectedBufferFrames = 0;
        activeBufferInfoCount = 0;
        outputDescriptorOffset = 0;
        inputTapEnabled = false;
        selectedInputFirstChannel = -1;
        inputTapCallback = nullptr;
        inputTapContext = nullptr;
        outputReadyEnabled.store(false, std::memory_order_release);
        bufferInfos = {};
        capabilities = {};
        if (callbackSlotClaimed) {
            unpublishCallbackOwnerAndDrain();
        }
        return firstFailure;
    }

    std::uint32_t takeNotifications() noexcept {
        return notifications.exchange(0, std::memory_order_acq_rel);
    }

    IASIO* driver{nullptr};
    AsioRenderCallback renderCallback{nullptr};
    void* renderContext{nullptr};
    HWND systemWindowHandle{nullptr};
    DWORD ownerThreadId{0};
    bool comInitialized{false};
    bool comUsable{false};
    bool driverInitialized{false};
    bool buffersCreated{false};
    bool started_{false};
    bool callbackSlotClaimed{false};
    bool callbackLifetimeSafe{true};
    bool sampleRateChangedByUs{false};
    bool inputTapEnabled{false};
    std::size_t callbackSlot{0};
    double originalSampleRate_{0.0};
    long selectedBufferFrames{0};
    long activeBufferInfoCount{0};
    std::size_t outputDescriptorOffset{0};
    long selectedInputFirstChannel{-1};
    AsioCapabilities capabilities{};
    AsioInputTapCallback inputTapCallback{nullptr};
    void* inputTapContext{nullptr};
    std::array<ASIOBufferInfo, 4> bufferInfos{};
    std::vector<float> scratchLeft;
    std::vector<float> scratchRight;
    std::vector<float> inputScratchLeft;
    std::vector<float> inputScratchRight;
    std::atomic<std::uint32_t> notifications{0};
    std::atomic<std::uint64_t> callbackCounter{0};
    std::atomic<bool> outputReadyEnabled{false};
};

AsioHost::AsioHost() : impl_(std::make_unique<Impl>()) {}

AsioHost::~AsioHost() {
    if (impl_) {
        (void)impl_->close();
    }
}

AsioHostStatus AsioHost::open(AsioRenderCallback callback,
                              void* callbackContext,
                              void* systemWindowHandle,
                              const AsioInputTap* inputTap) {
    return impl_->open(callback, callbackContext, systemWindowHandle, inputTap);
}

AsioHostStatus AsioHost::start(long bufferFrames) {
    const auto threadStatus = impl_->checkThread();
    if (!threadStatus) {
        return threadStatus;
    }
    return impl_->start(bufferFrames);
}

AsioHostStatus AsioHost::stop() {
    return impl_->stop();
}

AsioHostStatus AsioHost::close() {
    return impl_->close();
}

const AsioCapabilities& AsioHost::capabilities() const noexcept {
    return impl_->capabilities;
}

bool AsioHost::isOpen() const noexcept {
    return impl_->driver != nullptr;
}

bool AsioHost::isStarted() const noexcept {
    return impl_->started_;
}

std::uint64_t AsioHost::callbackCount() const noexcept {
    return impl_->callbackCounter.load(std::memory_order_relaxed);
}

std::uint32_t AsioHost::takeNotifications() noexcept {
    return impl_->takeNotifications();
}

}  // namespace axe
