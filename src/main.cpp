#include "windows_audio.hpp"
#include "axe/asio_host.hpp"
#include "axe/audio_pipeline.hpp"
#include "diagnostics/signal_metrics.hpp"
#include <endpointvolume.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>
#include <atomic>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <memory>
#include <thread>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {
constexpr double kClockTargetHz = 48'000.0;
constexpr double kClockTolerance = 0.01;
constexpr std::wstring_view kNativeFasEndpointName = L"Speakers (Axe-Fx III)";
using Microsoft::WRL::ComPtr;
struct ConfigurationError : std::runtime_error {
    using std::runtime_error::runtime_error;
};
HANDLE shutdown_event{};
BOOL WINAPI control(DWORD event) {
    if(event==CTRL_C_EVENT || event==CTRL_BREAK_EVENT || event==CTRL_CLOSE_EVENT || event==CTRL_SHUTDOWN_EVENT) {
        SetEvent(shutdown_event); return TRUE;
    }
    return FALSE;
}
struct Event {
    HANDLE h{};
    explicit Event(const wchar_t* name=nullptr):h(CreateEventW(nullptr,TRUE,FALSE,name)) {
        if(!h)throw std::runtime_error("Event creation failed");
    }
    ~Event(){CloseHandle(h);}
};
struct HostWindow {
    HWND h=CreateWindowExW(0,L"STATIC",L"Axe-Fx III USB 7/8 Bridge",WS_OVERLAPPED,
                         0,0,0,0,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    HostWindow(){if(!h)throw std::runtime_error("Hidden ASIO host window creation failed");}
    ~HostWindow(){DestroyWindow(h);}
};
bool wait_stop(DWORD milliseconds) {
    DWORD result=MsgWaitForMultipleObjects(1,&shutdown_event,FALSE,milliseconds,QS_ALLINPUT);
    MSG message{};
    while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)) {TranslateMessage(&message);DispatchMessageW(&message);}
    return result==WAIT_OBJECT_0 || WaitForSingleObject(shutdown_event,0)==WAIT_OBJECT_0;
}
void require(axe::AsioHostStatus status) {
    if(!status) throw std::runtime_error(status.message+" (native="+std::to_string(status.nativeCode)+")");
}
struct ComScope {
    bool owns{};
    ComScope() {
        const HRESULT hr=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
        if(hr!=RPC_E_CHANGED_MODE)axe::win::check(hr,"Endpoint volume COM initialization");
        owns=SUCCEEDED(hr);
    }
    ~ComScope(){if(owns)CoUninitialize();}
};
struct RawEndpointLevels {
    std::array<float,2> gains{};
    bool muted{};
};
class RawEndpointVolumeReader {
public:
    explicit RawEndpointVolumeReader(const std::wstring& id) {
        ComPtr<IMMDeviceEnumerator> enumerator;
        axe::win::check(CoCreateInstance(__uuidof(MMDeviceEnumerator),nullptr,CLSCTX_ALL,IID_PPV_ARGS(&enumerator)),"Raw volume enumerator");
        ComPtr<IMMDevice> device;
        axe::win::check(enumerator->GetDevice(id.c_str(),&device),"Raw volume endpoint");
        axe::win::check(device->Activate(__uuidof(IAudioEndpointVolume),CLSCTX_ALL,nullptr,&volume_),"Raw endpoint volume controls");
    }
    RawEndpointLevels read() {
        RawEndpointLevels result{};BOOL mute{};
        axe::win::check(volume_->GetMute(&mute),"Read raw endpoint mute");result.muted=mute!=FALSE;
        UINT channels{};axe::win::check(volume_->GetChannelCount(&channels),"Read raw endpoint channel count");
        if(channels<2)throw std::runtime_error("Native FAS volume interface must expose two channels");
        for(UINT i=0;i<2;++i) {
            float db{};axe::win::check(volume_->GetChannelVolumeLevel(i,&db),"Read raw endpoint channel dB");
            if(std::isnan(db) || (std::isinf(db) && db>0))throw std::runtime_error("Raw endpoint channel level is invalid");
            result.gains[i]=static_cast<float>(std::pow(10.0,static_cast<double>(db)/20.0));
            if(!std::isfinite(result.gains[i]) || result.gains[i]<0)throw std::runtime_error("Raw endpoint channel gain is invalid");
        }
        return result;
    }
private:
    ComScope com_;
    ComPtr<IAudioEndpointVolume> volume_;
};
bool same_level(float current,float original) noexcept {
    return std::isfinite(current) && std::isfinite(original) &&
           std::abs(current-original)<=std::max(1.0e-6f,std::abs(original)*0.001f);
}
void print_asio(const axe::AsioCapabilities& c) {
    std::cout<<c.driverName<<": "<<c.inputChannelCount<<" inputs, "<<c.outputChannelCount<<" outputs, "
        <<c.currentSampleRate<<" Hz; buffer min/preferred/max="<<c.minimumBufferFrames<<'/'
        <<c.preferredBufferFrames<<'/'<<c.maximumBufferFrames<<"\n";
    for(const auto& channel:c.selectedOutputs)
        std::cout<<"USB "<<channel.channelNumber+1<<": "<<channel.name<<", "
            <<axe::asioSampleFormatName(channel.format)<<"\n";
    if(c.activeBufferFrames)std::cout<<"Active ASIO buffer="<<c.activeBufferFrames<<" frames ("<<c.activeBufferFrames/48.0
        <<"ms), driver output latency="<<c.outputLatencyFrames<<" frames; ASIOOutputReady="
        <<(c.outputReadySupported?"supported":"unavailable")<<"\n";
}
struct SilentProbe {
    std::atomic<uint64_t> callbacks{},frames{};
    static void render(void* p,float* left,float* right,uint32_t count) noexcept {
        auto& s=*static_cast<SilentProbe*>(p);
        std::fill_n(left,count,0.0f);std::fill_n(right,count,0.0f);
        s.frames.fetch_add(count,std::memory_order_relaxed);s.callbacks.fetch_add(1,std::memory_order_relaxed);
    }
};
struct Session {
    Event stop,ready,begin;
    std::unique_ptr<axe::AudioPipeline> pipeline;
    std::unique_ptr<axe::win::EndpointVolumeReader> volume;
    axe::AsioHost asio;
    std::thread capture;
    std::thread signal;
    std::atomic<bool> failed{false};
    std::atomic<bool> signal_failed{false};
    std::atomic<uint64_t> captured{},rendered{},callbacks{};
    std::atomic<float> left_gain{1},right_gain{1};
    std::string error;
    std::string signal_error;
    uint32_t period{};
    uint32_t capture_buffer_frames{};
    std::atomic<uint32_t> max_packet{};
    std::atomic<uint64_t> source_discontinuities{},packets{};
    std::atomic<uint64_t> source_silent_frames{};
    HRESULT low_latency_result{};
    bool measure_signal=false;
    double left_energy{},right_energy{};
    axe::diagnostic::SignalMetrics tones;
    ~Session() {
        SetEvent(stop.h);
        asio.stop();
        if(capture.joinable())capture.join();
        if(signal.joinable())signal.join();
        asio.close();
    }
    static void ingest(void* p,const float* data,uint32_t count,bool discontinuity) {
        auto& s=*static_cast<Session*>(p);
        if(discontinuity) {s.source_discontinuities.fetch_add(1,std::memory_order_relaxed);s.pipeline->notify_discontinuity();}
        auto previous=s.max_packet.load(std::memory_order_relaxed);if(count>previous)s.max_packet.store(count,std::memory_order_relaxed);
        s.packets.fetch_add(1,std::memory_order_relaxed);
        if(data) (void)s.pipeline->ingest(data,count);else {
            s.source_silent_frames.fetch_add(count,std::memory_order_relaxed);
            (void)s.pipeline->submit_silence(count);
        }
        s.captured.fetch_add(count,std::memory_order_relaxed);
    }
    static void render(void* p,float* left,float* right,uint32_t count) noexcept {
        auto& s=*static_cast<Session*>(p);s.pipeline->render(left,right,count);
        float l=s.left_gain.load(std::memory_order_relaxed),r=s.right_gain.load(std::memory_order_relaxed);
        for(uint32_t i=0;i<count;++i){left[i]*=l;right[i]*=r;}
        if(s.measure_signal)for(uint32_t i=0;i<count;++i){s.left_energy+=left[i]*left[i];s.right_energy+=right[i]*right[i];s.tones.sample(left[i],right[i]);}
        s.rendered.fetch_add(count,std::memory_order_relaxed);s.callbacks.fetch_add(1,std::memory_order_relaxed);
    }
    static void on_ready(void* p) {
        auto& s=*static_cast<Session*>(p);SetEvent(s.ready.h);
        HANDLE waits[]={s.stop.h,s.begin.h};WaitForMultipleObjects(2,waits,FALSE,INFINITE);
    }
};
void usage() {
    std::cout<<"Axe-Fx III USB 7/8 bridge\n"
        "  axe-usb78 --list\n  axe-usb78 --asio-probe\n"
        "  axe-usb78 --asio-test [--seconds 10] [--buffer N]\n"
        "  axe-usb78 --bridge-test --endpoint-id <Speakers (Axe-Fx III) ID> [--seconds 30] [--buffer N]\n"
        "  axe-usb78 --run [--endpoint-id ID] [--buffer N] [--queue-ms 13] [--seconds N]\n"
        "Only the native Speakers (Axe-Fx III) WASAPI endpoint is routed. Ctrl+C stops routing.\n"
        "USB 7/8 require INPUT USB connected to an output in the Axe preset.\n";
}
}
int wmain(int argc,wchar_t** argv) {
    std::cout<<std::unitbuf;
    try {
        std::wstring command=argc>1?argv[1]:L"--help", endpoint_id, stop_event_name;
        long buffer=128;uint32_t seconds=0,queue_ms=0;
        for(int i=2;i<argc;++i) {
            std::wstring arg=argv[i];
            if(i+1>=argc) throw std::runtime_error("Missing argument value");
            std::wstring value=argv[++i];
            if(arg==L"--endpoint-id") endpoint_id=value;
            else if(arg==L"--stop-event") {
                if(!stop_event_name.empty() || !value.starts_with(L"Local\\AxeFxUsb78Stop-") || value.size()>200)
                    throw std::runtime_error("Invalid panel stop-event name");
                stop_event_name=value;
            }
            else {
                size_t end{};auto number=std::stoul(value,&end);
                if(end!=value.size() || number>86400)throw std::runtime_error("Invalid numeric argument");
                if(arg==L"--seconds")seconds=static_cast<uint32_t>(number);
                else if(arg==L"--buffer")buffer=static_cast<long>(number);
                else if(arg==L"--queue-ms") {if(number<13 || number>100)throw std::runtime_error("queue-ms must be 13..100");queue_ms=static_cast<uint32_t>(number);}
                else throw std::runtime_error("Unknown option");
            }
        }
        if(command==L"--help") {usage();return 0;}
        if(!stop_event_name.empty() && command!=L"--run")
            throw std::runtime_error("--stop-event is only supported with --run");
        if(buffer!=0 && (buffer<8 || buffer>2048 || (buffer&(buffer-1))!=0))
            throw std::runtime_error("buffer must be zero (driver preference) or a power of two from 8 through 2048");
        if(command==L"--list") {
            for(auto& e:axe::win::enumerate_render()) std::cout<<(e.is_default?"* ":"  ")<<axe::win::utf8(e.name)
                <<"\n  "<<axe::win::utf8(e.id)<<"\n  "<<e.rate<<" Hz "<<e.channels<<"ch "<<e.bits
                <<"bit, shared period "<<e.period_frames<<", minimum "<<e.minimum_frames<<" frames\n";
            return 0;
        }
        if(command!=L"--run" && command!=L"--bridge-test" && command!=L"--asio-probe" && command!=L"--asio-test")throw std::runtime_error("Unknown command; use --help");
        if(command==L"--bridge-test" && endpoint_id.empty())throw std::runtime_error("--bridge-test requires an explicit native FAS --endpoint-id");
        if(command==L"--bridge-test" && !seconds)seconds=30;
        if(command==L"--bridge-test" && seconds<2)
            throw std::runtime_error("--bridge-test requires at least 2 seconds for startup and clock measurement");
        const bool is_route_command=command==L"--run" || command==L"--bridge-test";
        if(is_route_command && !endpoint_id.empty()) {
            const auto endpoints=axe::win::enumerate_render();
            const auto selected=std::find_if(endpoints.begin(),endpoints.end(),[&](const auto& e){return e.id==endpoint_id;});
            if(selected==endpoints.end())throw std::runtime_error("Explicit render endpoint ID is not active");
            if(selected->name!=kNativeFasEndpointName)
                throw std::runtime_error("Routing accepts only the exact native Speakers (Axe-Fx III) endpoint ID");
        }
        HANDLE mutex=CreateMutexW(nullptr,TRUE,L"Local\\AxeFxUsb78Bridge");
        if(!mutex)throw std::runtime_error("Cannot create single-instance mutex");
        bool existing=GetLastError()==ERROR_ALREADY_EXISTS;
        struct MutexOwner {HANDLE h;bool owned;~MutexOwner(){if(owned)ReleaseMutex(h);CloseHandle(h);}} mutex_owner{mutex,!existing};
        if(existing)throw std::runtime_error("Another Axe USB 7/8 bridge instance is running");
        Event quit(stop_event_name.empty()?nullptr:stop_event_name.c_str());shutdown_event=quit.h;SetConsoleCtrlHandler(control,TRUE);
        HostWindow window;
        struct HandlerOwner {~HandlerOwner(){SetConsoleCtrlHandler(control,FALSE);}} handler;
        if(command==L"--asio-probe" || command==L"--asio-test") {
            SilentProbe test;axe::AsioHost host;require(host.open(SilentProbe::render,&test,window.h));
            if(command==L"--asio-probe") {print_asio(host.capabilities());require(host.close());return 0;}
            require(host.start(buffer));print_asio(host.capabilities());auto start=std::chrono::steady_clock::now();
            if(!seconds)seconds=10;
            while(!wait_stop(50) && std::chrono::steady_clock::now()-start<std::chrono::seconds(seconds)) {
                if(host.takeNotifications())throw std::runtime_error("ASIO requested reset during live test");
            }
            require(host.stop());require(host.close());
            if(std::chrono::steady_clock::now()-start<std::chrono::seconds(seconds)){std::cout<<"ASIO test canceled.\n";return 130;}
            std::cout<<"ASIO silence test: callbacks="<<test.callbacks.load()<<" frames="<<test.frames.load()<<"\n";
            if(test.frames.load()<48000ull*seconds*9/10)throw std::runtime_error("ASIO callback count below realtime expectation");
            std::cout<<"PASS: actual Fractal callback execution on USB 7/8\n";return 0;
        }
        auto process_start=std::chrono::steady_clock::now();uint32_t attempts{};bool routed=false;
        while(!wait_stop(0)) {
            if(seconds && std::chrono::steady_clock::now()-process_start>=std::chrono::seconds(seconds))break;
            try {
                auto endpoints=axe::win::enumerate_render();axe::win::Endpoint selected;bool found=false;
                for(auto& e:endpoints) {
                    bool matches=endpoint_id.empty()?e.name==kNativeFasEndpointName:e.id==endpoint_id;
                    if(matches) {if(found)throw std::runtime_error("Multiple matching endpoints; specify --endpoint-id");selected=e;found=true;}
                }
                if(!found)throw std::runtime_error("Native Speakers (Axe-Fx III) render endpoint is unavailable");
                Session s;require(s.asio.open(Session::render,&s,window.h));auto& c=s.asio.capabilities();
                s.measure_signal=command==L"--bridge-test";
                auto asio_frames=static_cast<size_t>(buffer?buffer:c.preferredBufferFrames);
                axe::AudioPipelineConfig config;
                s.capture=std::thread([&] {
                    try {axe::win::capture_loopback(selected.id,s.stop.h,Session::ingest,&s,s.period,Session::on_ready,&s.low_latency_result,&s.capture_buffer_frames);}
                    catch(const std::exception& e){s.error=e.what();s.failed.store(true,std::memory_order_release);SetEvent(s.ready.h);}
                });
                if(WaitForSingleObject(s.ready.h,5000)!=WAIT_OBJECT_0)throw std::runtime_error("Capture startup timed out");
                if(s.failed.load(std::memory_order_acquire))throw std::runtime_error(s.error);
                const size_t minimum_queue_frames=static_cast<size_t>(s.period)+asio_frames;
                config.target_fill_frames=queue_ms?static_cast<size_t>(queue_ms)*48:
                    std::max<size_t>(624,minimum_queue_frames);
                if(config.target_fill_frames<minimum_queue_frames) {
                    throw ConfigurationError("Queue target must cover at least one capture period plus one ASIO buffer ("+
                        std::to_string(minimum_queue_frames)+" frames; use a larger --queue-ms)");
                }
                config.preroll_frames=config.target_fill_frames;
                config.max_latency_frames=config.target_fill_frames+std::max<size_t>(4*asio_frames,2*s.period);
                config.capacity_frames=std::max<size_t>(8192,2*config.max_latency_frames);
                s.pipeline=std::make_unique<axe::AudioPipeline>(config);
                s.volume=std::make_unique<axe::win::EndpointVolumeReader>(selected.id);
                auto gain=s.volume->gains();s.left_gain.store(gain[0]);s.right_gain.store(gain[1]);
                std::array<float,2> test_gain{1.0f,1.0f};
                std::unique_ptr<RawEndpointVolumeReader> raw_volume;
                RawEndpointLevels initial_source_levels{};
                if(s.measure_signal) {
                    raw_volume=std::make_unique<RawEndpointVolumeReader>(selected.id);
                    initial_source_levels=raw_volume->read();
                    test_gain=initial_source_levels.gains;
                    if(initial_source_levels.muted || gain[0]<=0 || gain[1]<=0 || test_gain[0]<=0 || test_gain[1]<=0)
                        throw std::runtime_error("Unmute both native FAS endpoint channels before the signal diagnostic");
                    std::cout<<"Diagnostic raw captured endpoint gains L/R="<<test_gain[0]<<'/'<<test_gain[1]<<"\n";
                }
                require(s.asio.start(buffer));
                SetEvent(s.begin.h);
                auto routing_start=std::chrono::steady_clock::now();
                if(s.measure_signal)s.signal=std::thread([&]{try{axe::win::render_test_signal(selected.id,s.stop.h);}
                    catch(const std::exception& e){s.signal_error=e.what();s.signal_failed.store(true,std::memory_order_release);}});
                routed=true;
                std::cout<<"Routing "<<axe::win::utf8(selected.name)<<" -> Axe USB 7/8; capture period "<<s.period
                    <<" frames; capture capacity "<<s.capture_buffer_frames<<"; queue target "<<config.target_fill_frames/48.0<<"ms\n";print_asio(c);
                if(FAILED(s.low_latency_result))std::cout<<"Minimum-period loopback unavailable (HRESULT 0x"<<std::hex
                    <<static_cast<uint32_t>(s.low_latency_result)<<std::dec<<"); using established shared engine period.\n";
                auto report=std::chrono::steady_clock::now();
                auto progress=report;uint64_t last_callbacks=s.callbacks.load();
                auto capture_progress=report;uint64_t last_captured=s.captured.load(std::memory_order_relaxed);
                bool warmup_recorded=false;axe::AudioPipelineStats warmup_stats{};uint64_t overloads{};
                uint64_t warmup_source_discontinuities{},warmup_source_silent_frames{};
                uint64_t clock_captured_start{},clock_rendered_start{};
                uint64_t clock_captured_end{},clock_rendered_end{};
                std::chrono::steady_clock::time_point clock_measurement_start{},clock_measurement_end{};
                bool clock_measurement_started=false,clock_measurement_ended=false;
                bool completed_duration=false;
                while(!wait_stop(50)) {
                    auto timed_start=s.measure_signal?routing_start:process_start;
                    if(seconds && std::chrono::steady_clock::now()-timed_start>=std::chrono::seconds(seconds)){
                        completed_duration=true;
                        if(s.measure_signal && clock_measurement_started) {
                            clock_captured_end=s.captured.load(std::memory_order_relaxed);
                            clock_rendered_end=s.rendered.load(std::memory_order_relaxed);
                            clock_measurement_end=std::chrono::steady_clock::now();
                            clock_measurement_ended=true;
                        }
                        SetEvent(shutdown_event);break;
                    }
                    if(s.failed.load(std::memory_order_acquire))throw std::runtime_error(s.error);
                    if(s.signal_failed.load(std::memory_order_acquire))throw std::runtime_error(s.signal_error);
                    if(std::chrono::steady_clock::now()-routing_start>=std::chrono::seconds(5))attempts=0;
                    if(s.measure_signal) {
                        const auto current=raw_volume->read();
                        if(current.muted!=initial_source_levels.muted || current.muted ||
                           !same_level(current.gains[0],test_gain[0]) || !same_level(current.gains[1],test_gain[1]))
                            throw std::runtime_error("Native FAS master/channel volume or mute changed during the signal diagnostic");
                    }
                    gain=s.volume->gains();s.left_gain.store(gain[0],std::memory_order_relaxed);s.right_gain.store(gain[1],std::memory_order_relaxed);
                    auto flags=s.asio.takeNotifications();
                    if(flags&axe::asio_notify_overload) {
                        ++overloads;
                        if(!s.measure_signal)std::cerr<<"ASIO reported overload; session count="<<overloads<<"\n";
                    }
                    if(flags&(axe::asio_notify_reset|axe::asio_notify_resync|axe::asio_notify_buffer_size_changed|axe::asio_notify_sample_rate_changed))
                        throw std::runtime_error("ASIO state changed; restarting complete session");
                    auto callbacks=s.callbacks.load(std::memory_order_relaxed);
                    if(callbacks!=last_callbacks) {last_callbacks=callbacks;progress=std::chrono::steady_clock::now();}
                    else if(std::chrono::steady_clock::now()-progress>std::chrono::seconds(2))
                        throw std::runtime_error("ASIO callbacks stalled; reopening device");
                    auto captured=s.captured.load(std::memory_order_relaxed);
                    if(captured!=last_captured) {last_captured=captured;capture_progress=std::chrono::steady_clock::now();}
                    else if(std::chrono::steady_clock::now()-capture_progress>std::chrono::seconds(2))
                        throw std::runtime_error("WASAPI capture stopped delivering packets; reopening device");
                    if(!warmup_recorded && std::chrono::steady_clock::now()-routing_start>=std::chrono::seconds(1)) {
                        warmup_stats=s.pipeline->stats();warmup_recorded=true;
                        warmup_source_discontinuities=s.source_discontinuities.load(std::memory_order_relaxed);
                        warmup_source_silent_frames=s.source_silent_frames.load(std::memory_order_relaxed);
                        if(s.measure_signal) {
                            clock_captured_start=s.captured.load(std::memory_order_relaxed);
                            clock_rendered_start=s.rendered.load(std::memory_order_relaxed);
                            clock_measurement_start=std::chrono::steady_clock::now();
                            clock_measurement_started=true;
                        }
                    }
                    if(std::chrono::steady_clock::now()-report>=std::chrono::seconds(5)) {
                        auto stats=s.pipeline->stats();
                        std::cout<<"captured="<<s.captured.load()<<" rendered="<<s.rendered.load()<<" queued="<<stats.buffered_frames
                            <<" silent_frames="<<stats.dropout_frames<<" overflow="<<stats.overflow_frames<<" stale="<<stats.stale_frames_discarded
                            <<" discontinuities="<<stats.discontinuities<<" clock_ppm="<<stats.correction_ppm
                            <<" asio_overloads="<<overloads<<"\n";
                        std::cout<<"source_packets="<<s.packets.load()<<" max_packet="<<s.max_packet.load()
                            <<" source_silent_frames="<<s.source_silent_frames.load()
                            <<" source_discontinuities="<<s.source_discontinuities.load()<<"\n";
                        report=std::chrono::steady_clock::now();
                    }
                }
                if(s.measure_signal) {
                    SetEvent(s.stop.h);require(s.asio.stop());if(s.capture.joinable())s.capture.join();if(s.signal.joinable())s.signal.join();
                    if(!completed_duration){std::cout<<"Bridge signal test canceled.\n";return 130;}
                    auto frames=s.rendered.load();double l=std::sqrt(s.left_energy/static_cast<double>(frames?frames:1));
                    double r=std::sqrt(s.right_energy/static_cast<double>(frames?frames:1));auto stats=s.pipeline->stats();
                    std::cout<<"Bridge signal test: frames="<<frames<<" L_rms="<<l<<" R_rms="<<r<<" ratio="<<(r?l/r:0)
                        <<" overflow="<<stats.overflow_frames<<" stale="<<stats.stale_frames_discarded<<" silent_frames="<<stats.dropout_frames<<"\n";
                    std::cout<<"Tone RMS: L997="<<s.tones.rms(0,0)<<" L1511="<<s.tones.rms(0,1)<<" R997="<<s.tones.rms(1,0)<<" R1511="<<s.tones.rms(1,1)<<"\n";
                    if(s.failed.load(std::memory_order_acquire))throw std::runtime_error(s.error);
                    if(s.signal_failed.load(std::memory_order_acquire))throw std::runtime_error(s.signal_error);
                    const double clock_elapsed=clock_measurement_ended
                        ?std::chrono::duration<double>(clock_measurement_end-clock_measurement_start).count():0.0;
                    const uint64_t clock_captured=clock_measurement_ended?clock_captured_end-clock_captured_start:0;
                    const uint64_t clock_rendered=clock_measurement_ended?clock_rendered_end-clock_rendered_start:0;
                    const double captured_rate=clock_elapsed>0.0?static_cast<double>(clock_captured)/clock_elapsed:0.0;
                    const double rendered_rate=clock_elapsed>0.0?static_cast<double>(clock_rendered)/clock_elapsed:0.0;
                    const auto rate_ok=[](double rate) {
                        return std::isfinite(rate) && rate>=kClockTargetHz*(1.0-kClockTolerance) &&
                               rate<=kClockTargetHz*(1.0+kClockTolerance);
                    };
                    std::cout<<"Bridge clock: "<<clock_captured<<" Windows capture frames ("<<std::fixed<<std::setprecision(2)
                        <<captured_rate<<" Hz), "<<clock_rendered<<" ASIO output frames ("<<rendered_rate
                        <<" Hz) over "<<clock_elapsed<<" seconds; target 48000 Hz +/-1%.\n";
                    const auto final_source_discontinuities=s.source_discontinuities.load(std::memory_order_relaxed);
                    const auto final_source_silent_frames=s.source_silent_frames.load(std::memory_order_relaxed);
                    const bool clean_after_warmup=warmup_recorded &&
                        stats.dropout_frames==warmup_stats.dropout_frames &&
                        stats.overflow_frames==warmup_stats.overflow_frames &&
                        stats.stale_frames_discarded==warmup_stats.stale_frames_discarded &&
                        stats.discontinuities==warmup_stats.discontinuities &&
                        stats.non_finite_samples_sanitized==warmup_stats.non_finite_samples_sanitized &&
                        final_source_discontinuities==warmup_source_discontinuities &&
                        final_source_silent_frames==warmup_source_silent_frames;
                    std::cout<<"Post-warmup counters: dropout="<<(stats.dropout_frames-warmup_stats.dropout_frames)
                        <<" overflow="<<(stats.overflow_frames-warmup_stats.overflow_frames)
                        <<" stale="<<(stats.stale_frames_discarded-warmup_stats.stale_frames_discarded)
                        <<" pipeline_discontinuity="<<(stats.discontinuities-warmup_stats.discontinuities)
                        <<" nonfinite="<<(stats.non_finite_samples_sanitized-warmup_stats.non_finite_samples_sanitized)
                        <<" source_discontinuity="<<(final_source_discontinuities-warmup_source_discontinuities)
                        <<" source_silent_frames="<<(final_source_silent_frames-warmup_source_silent_frames)<<"\n";
                    if(!clock_measurement_started || !clock_measurement_ended || !rate_ok(captured_rate) || !rate_ok(rendered_rate) ||
                       l<0.0001*test_gain[0] || l>0.001*test_gain[0] || r<0.00005*test_gain[1] || r>0.0005*test_gain[1] ||
                       !clean_after_warmup ||
                       overloads || !s.tones.matches_stereo_tones(test_gain[0],test_gain[1]))
                        throw std::runtime_error("Bridge signal/stability verification failed");
                    std::cout<<"PASS: real Windows render -> WASAPI -> pipeline -> Fractal ASIO callbacks. Physical USB arrival/speaker latency requires hardware loopback.\n";
                }
            } catch(const ConfigurationError&) {
                throw;
            } catch(const std::exception& e) {
                std::cerr<<"Routing unavailable: "<<e.what()<<"\n";
                if(command==L"--bridge-test" || seconds)return 1;
                attempts=std::min<uint32_t>(attempts+1,5);
                if(wait_stop(1000*attempts))break;
            }
        }
        std::cout<<"Routing stopped.\n";return routed?0:1;
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
