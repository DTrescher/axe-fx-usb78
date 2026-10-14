#pragma once
#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include <array>

namespace axe::win {
struct Endpoint {
    std::wstring id, name;
    uint32_t rate{}, channels{}, bits{}, period_frames{}, minimum_frames{};
    bool ieee_float{}, is_default{};
};
std::vector<Endpoint> enumerate_render();
std::string utf8(const std::wstring& s);
// Control-thread only. Measured native FAS loopback already includes master
// and channel attenuation, so that exact endpoint uses unity gains while
// unmuted. Other endpoints mirror their channel levels. Mute is always applied.
class EndpointVolumeReader {
public:
    explicit EndpointVolumeReader(const std::wstring& id);
    ~EndpointVolumeReader();
    std::array<float,2> gains();
    // True when the bridge must apply endpoint channel levels to loopback.
    bool must_mirror_endpoint_gain() const noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
void check(HRESULT hr, const char* operation);
using CaptureSink = void(*)(void*, const float*, uint32_t, bool discontinuity);
// Runs entirely on the calling capture thread. Stops promptly when stop is set.
// Only 48kHz stereo float endpoints are accepted; no hidden resampling/downmix.
// Zero uses AXE_USB78_ENGINE_FRAMES when set; otherwise the advertised minimum
// shared-mode period when AudioClient3 supports it. Legacy shared mode retains
// its device default when period selection is unavailable.
void capture_loopback(const std::wstring& id, HANDLE stop, CaptureSink sink, void* context,
                      uint32_t& actual_period, void(*ready)(void*) = nullptr,
                      HRESULT* low_latency_result = nullptr, uint32_t* capture_buffer_frames = nullptr,
                      uint32_t engine_period_frames = 0);
struct RenderTestTiming {
    uint32_t default_engine_frames{}, fundamental_frames{}, minimum_engine_frames{}, maximum_engine_frames{};
    uint32_t initialized_engine_frames{}, current_engine_frames{}, buffer_frames{};
    uint64_t event_wake_count{};
    double mean_event_interval_ms{}, minimum_event_interval_ms{}, maximum_event_interval_ms{};
    bool used_audio_client3{};
};
// Diagnostic only: default amplitudes are -66dBFS left 997Hz and -72dBFS right 1511Hz.
// The optional timing record is written only by the calling thread and may be read after it returns.
// Zero uses the same shared-mode minimum-period policy as capture_loopback.
// Each call uses a fresh session GUID; session_gain applies only to that new session.
void render_test_signal(const std::wstring& id, HANDLE stop,
                        float left_amplitude = 0.0005f, float right_amplitude = 0.00025f,
                        RenderTestTiming* timing = nullptr, uint32_t engine_period_frames = 0,
                        float session_gain = 1.0f);
}
