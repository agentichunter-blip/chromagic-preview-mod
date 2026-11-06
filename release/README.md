# Release artifacts

This source tree does not configure a published release. The upstream 1.0.1 manifest was removed because its versions and hashes do not describe this mod.

Build `mcu/build/mcu_fw.bin` using the root README. It is an MCU application image, installed at `0x10000`, not a complete flash dump. No custom FPGA `.fs` is needed; the tested FPGA is upstream 18.38.

Before publishing any future binary, provide its matching source, SHA-256 checksum, version, and installation instructions. Never include private flash dumps, saves, game libraries, downloaded media packs, or local build logs.
