# Architecture

## Audio route

```text
Windows shared-mode render endpoint
        ↓
WASAPI endpoint loopback capture
        ↓
bounded stereo SPSC queue and rate correction
        ↓
FAS ASIO output channels 7/8
        ↓
Axe-Fx USB input block in the selected preset
```

The bridge captures the existing **Speakers (Axe-Fx III)** endpoint. It does not install an endpoint or driver. The endpoint's original USB 1/2 playback continues while the bridge sends an additional copy on USB 7/8.

## Threads and audio ownership

The WASAPI capture thread is the single producer for the stereo queue. The FAS ASIO callback is its single consumer. `axe::AudioPipeline` owns the fixed-capacity queue, resampling state, and stream counters. It is constructed before audio starts; callback operations use preallocated storage and bounded loops.

The callback owns its read cursor and interpolation state. A producer-side discontinuity request is consumed by the callback, which discards stale queued frames and resets its own resampler state. The capture and callback rates are corrected against queue occupancy to accommodate independent device clocks.

The control and supervisor paths handle device state, ASIO notifications, restart attempts, user settings, and status output. They stay outside the real-time callback.

## Source map

- `include/axe/stereo_spsc_ring.hpp` — fixed-capacity single-producer/single-consumer stereo queue.
- `include/axe/audio_pipeline.hpp` — queueing, drift correction, resampling, and stream statistics.
- `include/axe/asio_host.hpp` and `src/asio_host.cpp` — FAS ASIO host, output channel selection, and sample-format conversion.
- `src/windows_audio.hpp` and `src/windows_audio.cpp` — WASAPI endpoint discovery, loopback capture, and volume/mute handling.
- `src/main.cpp` — bridge lifecycle, command-line diagnostics, and supervision.
- `src/tray_main.cpp` — Windows control panel and tray behavior.
- `tests/diagnostics/` — standalone hardware checks; these are not run by CTest.

## Audio callback contract

Changes to `AudioPipeline::render` or the ASIO callback should preserve these properties:

- no heap allocation, mutex acquisition, file or console I/O, or waits;
- all output samples are initialized, including underrun silence;
- work is bounded by the configured callback size and filter length;
- producer and consumer never move each other's queue cursor.

See [testing](testing.md) for the hardware-independent and device-backed checks.
