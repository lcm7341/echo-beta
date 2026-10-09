# Mobile launcher and rendering

The Echo icon stays enabled on Android and iOS, including when loading settings
that previously disabled it. Theme → Icon Size adjusts it from 20 to 64 game units.
Its size and dragged position are saved separately.

During mobile capture, Pause / Resume, Stop, and Exit Level remain at the top of
the screen. Pause freezes simulation, actions, video frames, and captured audio;
it restores the normal display cadence until resumed. Stop finalizes the capture.
Exit Level stops the capture before leaving. These controls are drawn on the
physical screen after video capture and are excluded from the saved frames.
Screen touch coordinates use the physical view even when video dimensions differ.

The FFmpeg API 2.0 WAV path opens its decoder before applying audio stream
parameters, causing the PCM decoder's “Failed to open codec: invalid argument”.
Echo instead reads captured floats into interleaved stereo and calls the raw
mixer, which determines the input sample rate from video duration. Mono is
duplicated to stereo; invalid samples are replaced with silence. Failed muxes
retain a WAV (or the original raw file if WAV conversion also fails), and the
original video is preserved during replacement.

Android tries hardware H.264 first for the default codec and falls back to
available software encoders. Software H.264 has Fastest / Fast / Balanced presets
(ultrafast / veryfast / medium). The default is Fast. Android's FFmpeg API lacks a
preset field, so a scoped codec-open hook applies options only while Echo opens
its own x264 encoder. Other mods and audio codecs keep their options. Faster
presets reduce compression efficiency at a given bitrate.

Capture reuses buffers returned by the encoding worker and avoids repeating the
video view resize for every frame in a batch. Default builds use RelWithDebInfo;
Android packages have been rebuilt with -O2 rather than the previous unoptimized
Debug configuration. No fast-math options are used.

SSB Fix now applies the scroll-drift compensation when enabled (previously the
toggle was inverted), and defaults to enabled for new settings. Color Fix stays
requested across codec fallback instead of silently disabling itself. Desktop
GPU NV12 capture already converts RGB to limited-range BT.709 and no longer
applies a second BT.601-to-BT.709 conversion.

## Android physics patch

ARM64 now replaces the physics iteration calculation at library-relative
0x89d9f8 with a load of Echo's step count, clamped to at least one. It resumes
at 0x89da20 with the integer count on the stack and its double equivalent in d0.
Time warp and the game's three per-step delta divisions remain intact. This
enables TPS below 240 and batched updates with Lock TPS off. The patch first
checks all 40 original instruction bytes; batching and low TPS remain disabled
if verification or installation fails. Death updates refresh the vanilla count.

The instructions were extracted from the phone's ARM64 library (SHA-256
dda3752ab3a912fd2293561e157ecc98eca799056abd9223a331c1c514175df7).
Android reports versionName 2.2.144 / versionCode 41; the update entry 0x89d808
matches the project's 2.2081 symbols. ARM32 retains its fallback because no real
ARM32 library has been supplied. Runtime gameplay remains to be tested.

## Validation

Build Windows, Android ARM64, and Android ARM32 with CMake. The portable audio
regression test covers mono, stereo, multiple channels, non-finite samples,
clipping, a block boundary, invalid channels, missing files, and partial frames:

```sh
c++ -std=c++23 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -Isrc tests/stereo_audio.cpp -o /tmp/echo-stereo-test
/tmp/echo-stereo-test
```

`tests/arm64_step_patch.py <libcocos2dcpp.so>` uses Unicorn and pyelftools to
verify the original instruction signature and execute the replacement followed
by the game's delta divisions. It covers zero and positive counts, TPS below
and above 240, and time warp below, at, and above one (63 cases).

An actual Android device is still required to verify the codec hook, hardware
encoder availability, pause/stop/exit touch behavior, and render throughput.
