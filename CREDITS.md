# Credits and source provenance

This project is a modification of existing work. The neutral name used for this repository's import commit identifies the packaging of this source snapshot; it does not claim authorship of the upstream code.

## ChroMagic and Chromatic

- **CursedToast** — creator of [ChroMagic](https://github.com/cursedtoast2/ChroMagic), the firmware this mod builds on. The starting point is ChroMagic 1.0.1, commit [`e611a1e`](https://github.com/cursedtoast2/ChroMagic/commit/e611a1ef36eb9aa93f706d836f5b0e46129d7137). Consult that repository for the original development history and contributor attribution.
- **ModRetro** — original Chromatic firmware and FPGA design on which ChroMagic is based.
- **This mod** — artwork and gameplay-preview browsing, related navigation/loading UI, media conversion tools, and associated fixes/tests described in the README. It is an independent modification, not an official ChroMagic or ModRetro release.

## FPGA, emulation, and boot ROM work

The upstream acknowledgments in [NOTICE](NOTICE) are retained:

- [Gameboy_MiSTer contributors](https://github.com/MiSTer-devel/Gameboy_MiSTer), whose core is included in the FPGA source.
- [Gameboy for MiST contributors](https://github.com/mist-devel/gameboy), from which the MiSTer core was ported.
- [SameBoy contributors](https://github.com/LIJI32/SameBoy/), whose boot ROM sources were adapted upstream; their MIT notices remain in the relevant source files.
- **rayjt9**, credited upstream for boot ROM palette improvements.
- **GOWIN Semiconductor**, acknowledged in the upstream FPGA notices.

Additional authors are identified in the source headers. Those notices are preserved; this list is not a replacement for them or an exhaustive list of contributors.

## Dependencies and visual assets

- **Espressif ESP-IDF** and **LVGL** provide the firmware framework and UI library. Build dependencies retain their own licenses and notices.
- Included Game Boy / Game Boy Color wordmark sources and provenance are documented in [the system logo credits](mcu/tools/platform-logos/README.md). Nintendo trademarks remain Nintendo's.
- No third-party cover or gameplay-video pack is included in this repository.

## Licenses and provenance

See [mcu/LICENSE](mcu/LICENSE), [fpga/LICENSE](fpga/LICENSE), and individual source notices. This mod does not replace upstream license terms or remove upstream attribution.

[SOURCE_SNAPSHOT.json](SOURCE_SNAPSHOT.json) records the upstream component revisions. This repository starts with a clean source import rather than reproducing the upstream Git history; the original ChroMagic repository linked above remains the reference for that history.
