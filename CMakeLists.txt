cmake_minimum_required(VERSION 3.21)
project(AxeFxUsb78UserMode VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Keep release builds self-contained so a per-user install does not need to
# install the Visual C++ runtime on the whole machine.
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

enable_testing()
find_package(Threads REQUIRED)

add_library(axe_options INTERFACE)
target_include_directories(axe_options INTERFACE include)

if(MSVC)
    target_compile_options(axe_options INTERFACE /W4 /permissive- /EHsc /utf-8)
    target_compile_definitions(
        axe_options
        INTERFACE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE
    )
else()
    target_compile_options(axe_options INTERFACE -Wall -Wextra -Wpedantic)
endif()

# Hardware-independent tests
add_executable(audio_pipeline_tests tests/audio_pipeline_tests.cpp)
target_link_libraries(audio_pipeline_tests PRIVATE axe_options Threads::Threads)
add_test(NAME audio_pipeline COMMAND audio_pipeline_tests)
set_tests_properties(audio_pipeline PROPERTIES TIMEOUT 180)

add_executable(signal_metrics_tests tests/signal_metrics_tests.cpp)
target_include_directories(signal_metrics_tests PRIVATE src/diagnostics)
target_link_libraries(signal_metrics_tests PRIVATE axe_options)
add_test(NAME signal_metrics COMMAND signal_metrics_tests)

if(WIN32)
    # Windows settings tests
    add_executable(tray_settings_tests tests/tray_settings_tests.cpp)
    target_include_directories(tray_settings_tests PRIVATE src)
    target_link_libraries(tray_settings_tests PRIVATE axe_options)
    add_test(NAME tray_settings COMMAND tray_settings_tests)

    # Windows audio and ASIO implementation
    add_library(windows_audio src/windows_audio.cpp)
    target_link_libraries(windows_audio PUBLIC axe_options ole32 uuid avrt)

    add_library(asio_host src/asio_host.cpp)
    target_include_directories(asio_host PRIVATE third_party/asio)
    target_link_libraries(asio_host PUBLIC axe_options ole32 advapi32 user32)

    add_executable(axe-usb78 src/main.cpp)
    target_link_libraries(axe-usb78 PRIVATE windows_audio asio_host)

    add_executable(axe-usb78-panel WIN32 src/tray_main.cpp)
    target_link_libraries(
        axe-usb78-panel
        PRIVATE axe_options user32 gdi32 shell32 ole32
    )

    # Built explicitly by the tray recovery integration test.
    add_executable(
        axe-usb78-panel-delayed-tray-test
        WIN32 EXCLUDE_FROM_ALL
        src/tray_main.cpp
    )
    target_compile_definitions(
        axe-usb78-panel-delayed-tray-test
        PRIVATE AXE_PANEL_TEST_TRAY_DELAY_MS=4000
    )
    target_link_libraries(
        axe-usb78-panel-delayed-tray-test
        PRIVATE axe_options user32 gdi32 shell32 ole32
    )

    # Standalone hardware diagnostics. These are not run by CTest.
    add_executable(axe-device-probe tests/diagnostics/device_probe.cpp)
    target_include_directories(axe-device-probe PRIVATE src)
    target_link_libraries(axe-device-probe PRIVATE windows_audio)

    add_executable(axe-fas-shared-mix-test tests/diagnostics/shared_mix_test.cpp)
    target_include_directories(axe-fas-shared-mix-test PRIVATE src)
    target_link_libraries(axe-fas-shared-mix-test PRIVATE windows_audio)

    add_executable(axe-fas-volume-test tests/diagnostics/fas_volume_test.cpp)
    target_include_directories(axe-fas-volume-test PRIVATE src)
    target_link_libraries(axe-fas-volume-test PRIVATE windows_audio)

    add_executable(axe-fas-feasibility tests/diagnostics/feasibility_test.cpp)
    target_include_directories(axe-fas-feasibility PRIVATE src)
    target_link_libraries(axe-fas-feasibility PRIVATE asio_host windows_audio)

    # Compile the real ASIO host with a test-only driver factory.
    add_library(asio_host_testable STATIC src/asio_host.cpp)
    target_include_directories(
        asio_host_testable
        PRIVATE third_party/asio tests/support
    )
    target_compile_definitions(asio_host_testable PUBLIC AXE_ASIO_TESTING)
    target_link_libraries(
        asio_host_testable
        PUBLIC axe_options ole32 advapi32 user32
    )

    add_executable(asio_format_tests tests/asio_format_tests.cpp)
    target_include_directories(
        asio_format_tests
        PRIVATE third_party/asio tests/support
    )
    target_link_libraries(asio_format_tests PRIVATE asio_host_testable)
    add_test(NAME asio_formats COMMAND asio_format_tests)
endif()
