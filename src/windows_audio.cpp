#include "windows_audio.hpp"
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <avrt.h>
#include <endpointvolume.h>
#include <audiopolicy.h>
#include <wrl/client.h>
#include <ksmedia.h>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <cmath>
#include <algorithm>
#include <string_view>
#include <charconv>
#include <optional>
#include <system_error>

namespace axe::win {
using Microsoft::WRL::ComPtr;
void check(HRESULT hr, const char* operation) {
    if (FAILED(hr)) {
        std::ostringstream s; s << operation << " failed (HRESULT 0x" << std::hex
            << static_cast<uint32_t>(hr) << ")";
        throw std::runtime_error(s.str());
    }
}
static std::optional<uint32_t> engine_period_override(uint32_t explicit_frames) {
    if(explicit_frames != 0)return explicit_frames;
    char value[64]{};
    SetLastError(ERROR_SUCCESS);
    const DWORD length=GetEnvironmentVariableA("AXE_USB78_ENGINE_FRAMES",value,sizeof(value));
    if(length==0 && GetLastError()==ERROR_ENVVAR_NOT_FOUND)return std::nullopt;
    if(length==0 || length>=sizeof(value))
        throw std::runtime_error("AXE_USB78_ENGINE_FRAMES must be a positive base-10 frame count");
    const std::string_view text(value,length);
    uint32_t frames{};
    const auto parsed=std::from_chars(text.data(),text.data()+text.size(),frames,10);
    if(text.empty() || parsed.ec!=std::errc{} || parsed.ptr!=text.data()+text.size() || frames==0)
        throw std::runtime_error("AXE_USB78_ENGINE_FRAMES must be a positive base-10 frame count with no whitespace or trailing characters");
    return frames;
}
static uint32_t select_engine_period(const std::optional<uint32_t>& requested,
                                    uint32_t minimum_frames,uint32_t fundamental_frames,
                                    uint32_t maximum_frames) {
    if(minimum_frames==0 || fundamental_frames==0 || maximum_frames<minimum_frames)
        throw std::runtime_error("AudioClient3 returned invalid shared-mode engine period limits");
    const uint32_t frames=requested.value_or(minimum_frames);
    if(frames<minimum_frames || frames>maximum_frames || frames%fundamental_frames!=0) {
        std::ostringstream message;
        message << "Requested engine period " << frames << " frames must be within ["
                << minimum_frames << ", " << maximum_frames << "] and a multiple of "
                << fundamental_frames;
        throw std::runtime_error(message.str());
    }
    return frames;
}
struct ComScope {
    bool owns{};
    explicit ComScope(DWORD mode=COINIT_MULTITHREADED) {
        HRESULT hr=CoInitializeEx(nullptr,mode);
        if(hr!=RPC_E_CHANGED_MODE)check(hr,"COM initialization");
        owns=SUCCEEDED(hr);
    }
    ~ComScope() { if(owns)CoUninitialize(); }
};
struct WaveOwner { WAVEFORMATEX* p{}; ~WaveOwner() { CoTaskMemFree(p); } };
struct Handle { HANDLE p{}; ~Handle() { if(p) CloseHandle(p); } };
struct Mmcss {
    HANDLE h{};
    Mmcss() { DWORD task{}; h=AvSetMmThreadCharacteristicsW(L"Pro Audio", &task);
        if(h) AvSetMmThreadPriority(h, AVRT_PRIORITY_HIGH); }
    ~Mmcss() { if(h) AvRevertMmThreadCharacteristics(h); }
};
static bool is_native_fas_endpoint(IMMDevice* device) {
    ComPtr<IPropertyStore> properties;
    check(device->OpenPropertyStore(STGM_READ, &properties), "Endpoint gain-policy properties");
    PROPVARIANT friendly_name;PropVariantInit(&friendly_name);
    const HRESULT hr=properties->GetValue(PKEY_Device_FriendlyName,&friendly_name);
    const bool has_friendly_name=SUCCEEDED(hr) && friendly_name.vt==VT_LPWSTR && friendly_name.pwszVal;
    const bool matches=has_friendly_name && std::wstring_view(friendly_name.pwszVal)==L"Speakers (Axe-Fx III)";
    PropVariantClear(&friendly_name);
    if(FAILED(hr))check(hr,"Endpoint gain-policy friendly name");
    if(!has_friendly_name)
        throw std::runtime_error("Endpoint friendly name is unavailable; cannot determine its volume policy");
    return matches;
}
struct EndpointVolumeReader::Impl {
    ComScope com{COINIT_APARTMENTTHREADED};
    ComPtr<IAudioEndpointVolume> volume;
    bool softwareGainAlreadyCaptured{};
    explicit Impl(const std::wstring& id) {
        ComPtr<IMMDeviceEnumerator> enumerator;
        check(CoCreateInstance(__uuidof(MMDeviceEnumerator),nullptr,CLSCTX_ALL,IID_PPV_ARGS(&enumerator)),"Volume enumerator");
        ComPtr<IMMDevice> device;check(enumerator->GetDevice(id.c_str(),&device),"Volume endpoint");
        const bool isNativeFas=is_native_fas_endpoint(device.Get());
        check(device->Activate(__uuidof(IAudioEndpointVolume),CLSCTX_ALL,nullptr,&volume),"Endpoint volume controls");
        DWORD hardware_support{};
        check(volume->QueryHardwareSupport(&hardware_support),"Endpoint volume hardware support");
        if(isNativeFas) {
            if((hardware_support&ENDPOINT_HARDWARE_SUPPORT_VOLUME)!=0)
                throw std::runtime_error("Native FAS endpoint volume support changed; verify loopback gain timing before playback");
            // Live measurement of Speakers (Axe-Fx III), driver 6.16.0.22086,
            // showed loopback already follows master and channel attenuation.
            softwareGainAlreadyCaptured=true;
        }
    }
};
EndpointVolumeReader::EndpointVolumeReader(const std::wstring& id):impl_(std::make_unique<Impl>(id)){}
EndpointVolumeReader::~EndpointVolumeReader()=default;
bool EndpointVolumeReader::must_mirror_endpoint_gain() const noexcept {
    return !impl_->softwareGainAlreadyCaptured;
}
std::array<float,2> EndpointVolumeReader::gains() {
    BOOL mute{};check(impl_->volume->GetMute(&mute),"Endpoint mute");
    if(mute)return {0,0};
    if(!must_mirror_endpoint_gain())return {1.0f,1.0f};
    UINT channels{};check(impl_->volume->GetChannelCount(&channels),"Endpoint volume channels");
    std::array<float,2> result{};
    for(UINT i=0;i<2;++i) {
        float db{};HRESULT hr=channels>i?impl_->volume->GetChannelVolumeLevel(i,&db):impl_->volume->GetMasterVolumeLevel(&db);
        check(hr,"Endpoint channel gain");result[i]=static_cast<float>(std::pow(10.0,static_cast<double>(db)/20.0));
        if(!std::isfinite(result[i]) || result[i]<0)result[i]=0;
    }
    return result;
}
static bool is_float(const WAVEFORMATEX* f) {
    return f->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
        (f->wFormatTag == WAVE_FORMAT_EXTENSIBLE && f->cbSize >= 22 &&
         reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(f)->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
}
std::string utf8(const std::wstring& s) {
    if(s.empty()) return {};
    int count=WideCharToMultiByte(CP_UTF8,0,s.data(),static_cast<int>(s.size()),nullptr,0,nullptr,nullptr);
    std::string out(count,'\0');
    WideCharToMultiByte(CP_UTF8,0,s.data(),static_cast<int>(s.size()),out.data(),count,nullptr,nullptr);
    return out;
}
std::vector<Endpoint> enumerate_render() {
    ComScope com;
    ComPtr<IMMDeviceEnumerator> enumerator;
    check(CoCreateInstance(__uuidof(MMDeviceEnumerator),nullptr,CLSCTX_ALL,IID_PPV_ARGS(&enumerator)),"Device enumeration");
    ComPtr<IMMDevice> def; std::wstring default_id;
    if(SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender,eConsole,&def))) {
        LPWSTR id{}; if(SUCCEEDED(def->GetId(&id))) { default_id=id; CoTaskMemFree(id); }
    }
    ComPtr<IMMDeviceCollection> collection;
    check(enumerator->EnumAudioEndpoints(eRender,DEVICE_STATE_ACTIVE,&collection),"Render endpoints");
    UINT count{}; check(collection->GetCount(&count),"Endpoint count");
    std::vector<Endpoint> result;
    for(UINT i=0;i<count;++i) {
        ComPtr<IMMDevice> device; check(collection->Item(i,&device),"Endpoint");
        Endpoint e; LPWSTR id{}; check(device->GetId(&id),"Endpoint ID"); e.id=id; CoTaskMemFree(id);
        e.is_default=e.id==default_id;
        ComPtr<IPropertyStore> props; check(device->OpenPropertyStore(STGM_READ,&props),"Properties");
        PROPVARIANT v; PropVariantInit(&v);
        if(SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName,&v)) && v.vt==VT_LPWSTR) e.name=v.pwszVal;
        PropVariantClear(&v);
        ComPtr<IAudioClient> client;
        if(SUCCEEDED(device->Activate(__uuidof(IAudioClient),CLSCTX_ALL,nullptr,&client))) {
            WaveOwner f;
            if(SUCCEEDED(client->GetMixFormat(&f.p))) {
                e.rate=f.p->nSamplesPerSec; e.channels=f.p->nChannels; e.bits=f.p->wBitsPerSample; e.ieee_float=is_float(f.p);
                ComPtr<IAudioClient3> low;
                UINT d{},u{},min{},max{};
                if(SUCCEEDED(client.As(&low)) && SUCCEEDED(low->GetSharedModeEnginePeriod(f.p,&d,&u,&min,&max))) {
                    e.period_frames=d; e.minimum_frames=min;
                }
            }
        }
        result.push_back(std::move(e));
    }
    return result;
}
void capture_loopback(const std::wstring& id,HANDLE stop,CaptureSink sink,void* context,
                     uint32_t& actual_period,void(*ready)(void*),HRESULT* low_latency_result,
                     uint32_t* capture_buffer_frames,uint32_t engine_period_frames) {
    auto requested_engine_period=engine_period_override(engine_period_frames);
    ComScope com;
    ComPtr<IMMDeviceEnumerator> enumerator;
    check(CoCreateInstance(__uuidof(MMDeviceEnumerator),nullptr,CLSCTX_ALL,IID_PPV_ARGS(&enumerator)),"Enumerator");
    ComPtr<IMMDevice> device; check(enumerator->GetDevice(id.c_str(),&device),"Capture endpoint");
    ComPtr<IAudioClient> client;
    check(device->Activate(__uuidof(IAudioClient),CLSCTX_ALL,nullptr,&client),"Audio client");
    WaveOwner f; check(client->GetMixFormat(&f.p),"Mix format");
    if(f.p->nSamplesPerSec!=48000 || f.p->nChannels!=2 || f.p->wBitsPerSample!=32 || !is_float(f.p) || f.p->nBlockAlign!=8)
        throw std::runtime_error("Capture endpoint must use stereo 48000Hz 32-bit float. Configure its shared-mode format first.");
    constexpr DWORD flags=AUDCLNT_STREAMFLAGS_LOOPBACK|AUDCLNT_STREAMFLAGS_EVENTCALLBACK|AUDCLNT_STREAMFLAGS_NOPERSIST;
    // IAudioClient3 only accepts EVENTCALLBACK, not LOOPBACK. A silent render
    // stream requests the engine period; the standard loopback stream taps it.
    ComPtr<IAudioClient3> low;ComPtr<IAudioClient> anchor;
    ComPtr<IAudioRenderClient> anchor_render;
    Handle anchor_event{CreateEventW(nullptr,FALSE,FALSE,nullptr)};
    UINT anchor_capacity{},d{},fundamental{},min{},max{};
    bool anchor_started=false;
    uint32_t selected_engine_period{};
    struct StopAnchor { IAudioClient*& p;~StopAnchor(){if(p)p->Stop();} };
    IAudioClient* started_anchor=nullptr;StopAnchor anchor_stop{started_anchor};
    if(low_latency_result)*low_latency_result=E_NOINTERFACE;
    check(device->Activate(__uuidof(IAudioClient),CLSCTX_ALL,nullptr,&anchor),"Engine period client");
    const bool has_low_latency_limits=SUCCEEDED(anchor.As(&low)) &&
        SUCCEEDED(low->GetSharedModeEnginePeriod(f.p,&d,&fundamental,&min,&max));
    if(has_low_latency_limits) {
        selected_engine_period=select_engine_period(requested_engine_period,min,fundamental,max);
        HRESULT hr=low->InitializeSharedAudioStream(AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                                    selected_engine_period,f.p,nullptr);
        if(low_latency_result)*low_latency_result=hr;
        if(SUCCEEDED(hr)) {
            if(!anchor_event.p)throw std::runtime_error("Engine period event creation failed");
            check(anchor->SetEventHandle(anchor_event.p),"Engine period event");
            check(anchor->GetBufferSize(&anchor_capacity),"Engine period buffer size");
            check(anchor->GetService(IID_PPV_ARGS(&anchor_render)),"Engine period render service");
            BYTE* silence{};check(anchor_render->GetBuffer(anchor_capacity,&silence),"Engine period initial silence");
            check(anchor_render->ReleaseBuffer(anchor_capacity,AUDCLNT_BUFFERFLAGS_SILENT),"Engine period initial release");
            check(anchor->Start(),"Engine period start");started_anchor=anchor.Get();anchor_started=true;
        } else if(requested_engine_period) {
            check(hr,"Requested engine period initialization");
        }
    } else if(requested_engine_period) {
        throw std::runtime_error("A requested engine period requires IAudioClient3 shared-mode period support");
    }
    REFERENCE_TIME def{},minimum{};check(client->GetDevicePeriod(&def,&minimum),"Device period");
    REFERENCE_TIME capture_duration=anchor_started?static_cast<REFERENCE_TIME>((static_cast<uint64_t>(selected_engine_period)*10000000+47999)/48000):0;
    check(client->Initialize(AUDCLNT_SHAREMODE_SHARED,flags,capture_duration,0,f.p,nullptr),"Loopback initialization");
    UINT buffer_frames{};check(client->GetBufferSize(&buffer_frames),"Loopback buffer size");
    if(capture_buffer_frames)*capture_buffer_frames=buffer_frames;
    actual_period=anchor_started ? selected_engine_period
        :static_cast<uint32_t>(def*48000/10000000);
    ComPtr<IAudioClient3> current;
    if(SUCCEEDED(client.As(&current))) {
        WaveOwner current_format;UINT current_frames{};
        // Observe the active endpoint period so callers can size queue and
        // recovery budgets from the device that is actually being captured.
        if(SUCCEEDED(current->GetCurrentSharedModeEnginePeriod(&current_format.p,&current_frames)))
            actual_period=std::max(actual_period,current_frames);
    }
    Handle event{CreateEventW(nullptr,FALSE,FALSE,nullptr)};
    if(!event.p) throw std::runtime_error("Capture event creation failed");
    check(client->SetEventHandle(event.p),"Capture event");
    ComPtr<IAudioCaptureClient> capture;
    check(client->GetService(IID_PPV_ARGS(&capture)),"Capture service");
    Mmcss priority;
    check(client->Start(),"Capture start");
    struct StopClient { IAudioClient* p; ~StopClient(){ p->Stop(); } } stopped{client.Get()};
    if(ready) ready(context);
    HANDLE waits[]={stop,event.p,anchor_event.p};
    while(true) {
        DWORD wait=WaitForMultipleObjects(anchor_started?3:2,waits,FALSE,1000);
        if(wait==WAIT_OBJECT_0) return;
        if(wait==WAIT_FAILED) throw std::runtime_error("Capture wait failed");
        if(anchor_started) {
            UINT padding{};check(anchor->GetCurrentPadding(&padding),"Engine period padding");
            if(padding<anchor_capacity) {
                UINT frames=anchor_capacity-padding;BYTE* silence{};
                check(anchor_render->GetBuffer(frames,&silence),"Engine period silence buffer");
                check(anchor_render->ReleaseBuffer(frames,AUDCLNT_BUFFERFLAGS_SILENT),"Engine period silence release");
            }
        }
        // Silent engines may not signal loopback events. Timeout also checks removal.
        UINT frames{}; check(capture->GetNextPacketSize(&frames),"Next capture packet");
        while(frames) {
            BYTE* data{}; DWORD packet_flags{}; UINT got{};
            check(capture->GetBuffer(&data,&got,&packet_flags,nullptr,nullptr),"Capture buffer");
            sink(context,(packet_flags&AUDCLNT_BUFFERFLAGS_SILENT)?nullptr:reinterpret_cast<const float*>(data),
                 got,(packet_flags&AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY)!=0);
            check(capture->ReleaseBuffer(got),"Release capture buffer");
            check(capture->GetNextPacketSize(&frames),"Next capture packet");
        }
    }
}
void render_test_signal(const std::wstring& id,HANDLE stop,float left_amplitude,float right_amplitude,
                        RenderTestTiming* timing,uint32_t engine_period_frames,float session_gain) {
    constexpr float maximum_test_amplitude=0.0005f*10.0f;
    if(!std::isfinite(left_amplitude) || !std::isfinite(right_amplitude) ||
       std::abs(left_amplitude)>maximum_test_amplitude || std::abs(right_amplitude)>maximum_test_amplitude)
        throw std::runtime_error("Diagnostic signal amplitudes must be finite and no louder than approximately -46dBFS peak");
    if(!std::isfinite(session_gain) || session_gain<0.0f || session_gain>1.0f)
        throw std::runtime_error("Diagnostic session gain must be finite and within [0, 1]");
    auto requested_engine_period=engine_period_override(engine_period_frames);
    ComScope com;
    ComPtr<IMMDeviceEnumerator> enumerator;
    check(CoCreateInstance(__uuidof(MMDeviceEnumerator),nullptr,CLSCTX_ALL,IID_PPV_ARGS(&enumerator)),"Test enumerator");
    ComPtr<IMMDevice> device; check(enumerator->GetDevice(id.c_str(),&device),"Test endpoint");
    ComPtr<IAudioClient> client;
    check(device->Activate(__uuidof(IAudioClient),CLSCTX_ALL,nullptr,&client),"Test audio client");
    WaveOwner f; check(client->GetMixFormat(&f.p),"Test mix format");
    if(f.p->nSamplesPerSec!=48000 || f.p->nChannels!=2 || f.p->wBitsPerSample!=32 || !is_float(f.p))
        throw std::runtime_error("Test endpoint requires stereo 48kHz float");
    constexpr DWORD event_flags=AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
    constexpr DWORD legacy_flags=AUDCLNT_STREAMFLAGS_EVENTCALLBACK|AUDCLNT_STREAMFLAGS_NOPERSIST;
    GUID session_id{};
    check(CoCreateGuid(&session_id),"Test audio session GUID");
    ComPtr<IAudioClient3> low;
    UINT default_frames{},fundamental_frames{},minimum_frames{},maximum_frames{};
    bool initialized_low_latency=false;
    uint32_t selected_engine_period{};
    const bool has_low_latency_limits=SUCCEEDED(client.As(&low)) &&
        SUCCEEDED(low->GetSharedModeEnginePeriod(f.p,&default_frames,&fundamental_frames,&minimum_frames,&maximum_frames));
    if(has_low_latency_limits) {
        if(timing) {
            timing->default_engine_frames=default_frames;
            timing->fundamental_frames=fundamental_frames;
            timing->minimum_engine_frames=minimum_frames;
            timing->maximum_engine_frames=maximum_frames;
        }
        selected_engine_period=select_engine_period(requested_engine_period,minimum_frames,
                                                     fundamental_frames,maximum_frames);
        HRESULT hr=low->InitializeSharedAudioStream(event_flags,selected_engine_period,f.p,&session_id);
        if(SUCCEEDED(hr)) initialized_low_latency=true;
        else if(requested_engine_period)check(hr,"Requested test render engine period initialization");
    } else if(requested_engine_period) {
        throw std::runtime_error("A requested engine period requires IAudioClient3 shared-mode period support");
    }
    if(!initialized_low_latency)
        check(client->Initialize(AUDCLNT_SHAREMODE_SHARED,legacy_flags,0,0,f.p,&session_id),"Test render initialization");
    if(timing)timing->used_audio_client3=initialized_low_latency;
    Handle event{CreateEventW(nullptr,FALSE,FALSE,nullptr)};
    if(!event.p) throw std::runtime_error("Test event creation failed");
    check(client->SetEventHandle(event.p),"Test render event");
    UINT capacity{}; check(client->GetBufferSize(&capacity),"Test render size");
    if(timing) {
        timing->buffer_frames=capacity;
        timing->initialized_engine_frames=initialized_low_latency?selected_engine_period:0;
        if(initialized_low_latency) {
            WAVEFORMATEX* current_format{};UINT current_frames{};
            if(SUCCEEDED(low->GetCurrentSharedModeEnginePeriod(&current_format,&current_frames))) {
                timing->current_engine_frames=current_frames;
                CoTaskMemFree(current_format);
            }
        }
    }
    ComPtr<IAudioRenderClient> render; check(client->GetService(IID_PPV_ARGS(&render)),"Test render service");
    ComPtr<ISimpleAudioVolume> session_volume;
    check(client->GetService(IID_PPV_ARGS(&session_volume)),"Test session volume service");
    check(session_volume->SetMasterVolume(session_gain,nullptr),"Set test session volume");
    uint64_t sample{};
    auto fill=[&](UINT frames) {
        if(!frames) return;
        BYTE* data{}; check(render->GetBuffer(frames,&data),"Test render buffer");
        auto p=reinterpret_cast<float*>(data);
        constexpr double tau=6.2831853071795864769;
        for(UINT i=0;i<frames;++i,++sample) {
            p[2*i]=left_amplitude*static_cast<float>(std::sin(tau*997.0*static_cast<double>(sample)/48000.0));
            p[2*i+1]=right_amplitude*static_cast<float>(std::sin(tau*1511.0*static_cast<double>(sample)/48000.0));
        }
        check(render->ReleaseBuffer(frames,0),"Test render release");
    };
    fill(capacity);
    check(client->Start(),"Test signal start");
    struct StopClient { IAudioClient* p; ~StopClient(){p->Stop();} } stopped{client.Get()};
    Mmcss priority; HANDLE waits[]={stop,event.p};
    LARGE_INTEGER qpc_frequency{},last_event{};QueryPerformanceFrequency(&qpc_frequency);
    uint64_t interval_count=0;double interval_sum=0,interval_min=0,interval_max=0;
    while(true) {
        DWORD wait=WaitForMultipleObjects(2,waits,FALSE,1000);
        if(wait==WAIT_OBJECT_0) return;
        if(wait==WAIT_FAILED) throw std::runtime_error("Test signal wait failed");
        if(wait==WAIT_OBJECT_0+1 && timing) {
            LARGE_INTEGER now{};QueryPerformanceCounter(&now);
            if(last_event.QuadPart && qpc_frequency.QuadPart>0) {
                double ms=1000.0*static_cast<double>(now.QuadPart-last_event.QuadPart)/static_cast<double>(qpc_frequency.QuadPart);
                if(interval_count==0 || ms<interval_min)interval_min=ms;
                if(ms>interval_max)interval_max=ms;
                interval_sum+=ms;++interval_count;
            }
            last_event=now;
        }
        UINT padding{}; check(client->GetCurrentPadding(&padding),"Test render padding");
        if(padding<=capacity) fill(capacity-padding);
        if(timing) {
            timing->event_wake_count+=wait==WAIT_OBJECT_0+1?1u:0u;
            if(interval_count) {
                timing->mean_event_interval_ms=interval_sum/static_cast<double>(interval_count);
                timing->minimum_event_interval_ms=interval_min;
                timing->maximum_event_interval_ms=interval_max;
            }
        }
    }
}
}
