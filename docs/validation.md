# Validation status

This is an experimental MCU modification based on ChroMagic 1.0.1, tested with FPGA 18.38.

The art20 development installation was verified by the flash checksum and the running versions: MCU `v0.13.4-art20`, Chromatic `1.0.1-art20 (4.2)`, FPGA `18.38`. The FPGA was not changed. Earlier physical checks confirmed cover/video reads and game launches.

The 19-test host suite covers menu rendering and lifecycle, startup frame priming, folder scanning and BIOS filtering, tap/hold scrolling (including the 350 ms threshold and fade interruption), media formats and conversion, worker cancellation, SD access coordination, and save/load paths. It has passed both normally and under address/undefined-behavior sanitizers. Host tests use synthetic fixtures and do not establish compatibility with every game or SD card.

Remaining limitations: physical cold-start timing needs a dedicated check; sustained preview performance has not been benchmarked; extended gameplay/save testing across hardware and libraries remains incomplete. Build and host-test results do not replace those checks.

No hardware is flashed by the build or test scripts. A public binary release should identify its source revision, include SHA-256 checksums, and record the tested MCU/FPGA versions. No public release is configured in this source tree.
