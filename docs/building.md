# Build and test

Run commands from the repository root. The documented build environment is macOS/Linux with Bash, Python 3.12, CMake 3.22+, Ninja, FFmpeg (with libx264), and **ESP-IDF v5.3 / 5.3.0** with its ESP32 toolchain. `mcu/dependencies.lock` pins LVGL 8.4.0. ESP-IDF downloads managed components on the first build, so internet access is required then.

Install ESP-IDF v5.3 and its ESP32 tools using Espressif's setup instructions. Activate that installation's `export.sh` before building. Other ESP-IDF versions are rejected by the helper script.

Create a separate Python environment for the media tools and host tests:

```sh
python3 -m venv .venv-media
.venv-media/bin/python -m pip install -r mcu/tools/requirements-box-art.txt
```

After activating ESP-IDF, build firmware and run the host tests:

```sh
CHROMAGIC_HOST_PYTHON="$PWD/.venv-media/bin/python" \
  bash mcu/tools/build_box_art.sh
```

CMake, Ninja, and FFmpeg must be on PATH. The helper builds and tests only; it does not connect to a device. Output: `mcu/build/mcu_fw.bin`. Use the application-only installation steps in the root README.

Use a fresh source directory for reproducible builds. A pre-existing `mcu/sdkconfig` overrides `sdkconfig.defaults`; do not carry configuration from another revision into this build. The defaults identify art19 and use 16-bit RGB565 without byte swapping.

## Host tests only

Run an ESP-IDF build first to resolve LVGL. With ESP-IDF still activated, set the SDK path on macOS (the build helper already does this automatically):

```sh
if [ "$(uname -s)" = Darwin ]; then
  export SDKROOT="$(xcrun --sdk macosx --show-sdk-path)"
fi
```

Then configure and run the host tests:

```sh
cmake -S mcu/tests/host -B mcu/build-host -G Ninja \
  -DPython3_EXECUTABLE="$PWD/.venv-media/bin/python"
cmake --build mcu/build-host
ctest --test-dir mcu/build-host --output-on-failure
```

Tests include generated fixtures and real FFmpeg conversion; no commercial games or downloaded media are needed. The library tools accept user-provided files separately.

For address/undefined-behavior sanitizers with Clang or GCC:

```sh
cmake -S mcu/tests/host -B mcu/build-host-sanitize -G Ninja \
  -DPython3_EXECUTABLE="$PWD/.venv-media/bin/python" \
  -DCMAKE_C_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
cmake --build mcu/build-host-sanitize
ctest --test-dir mcu/build-host-sanitize --output-on-failure
```

Optional UI renders:

```sh
mkdir -p mcu/tests/host/evidence
CHROMATIC_TEST_RENDER_DIR="$PWD/mcu/tests/host/evidence" mcu/build-host/osd_test
```

Generated fonts and system logos are checked in, so their generators are not required for normal builds. Regenerating logos additionally requires CairoSVG and its native Cairo dependency.
