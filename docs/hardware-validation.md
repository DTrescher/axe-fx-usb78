# Hardware route validation

The application copies audio from the native **Speakers (Axe-Fx III)** Windows playback endpoint. A WASAPI loopback capture feeds the bridge, which writes stereo audio to FAS ASIO output channels 7 and 8.

## Test setup

For the normal listening route, place an Axe-Fx USB block in the preset and route it to the desired output. The original Windows endpoint continues to send its own audio to USB 1/2.

For an automated physical-return check, the test preset also needs a temporary INPUT USB → OUT 1 USB connection. That lets the ASIO test harness record the returned signal. OUT 3 can remain connected to speakers, but the test does not measure their acoustic output or end-to-end latency.

The diagnostic compares two quiet, distinct tones with the ASIO relay enabled, muted, and at different gains. A valid return follows the relay state and gain; the captured Windows loopback remains independent of those changes. The check also monitors stream timing, dropouts, overflows, and stale frames after startup.

## Results and limits

The route and hardware diagnostics have been exercised on an Axe-Fx III using the official FAS USB/ASIO driver. The recorded checks confirmed that the relay reached USB 7/8 independently of the endpoint's USB 1/2 playback, and a ten-minute stability run completed without post-startup streaming losses on that setup.

These results describe the tested hardware and driver configuration. They do not establish compatibility with every FAS driver version, exclusive-mode application, protected-content path, device reconnect, or suspend/resume cycle. The queue target is not a speaker-latency measurement.

See [build and validation](testing.md) for commands and setup details.
