# Credits

## This port

- **Glebos222** — PROTOTYPE port: testing, direction, releases.
- Port work (PROTOTYPE runtime additions, fixes, patches, launcher) was written with
  **Claude** (Anthropic).

## The ports this one is built on

This project is a fork of, and would not exist without:

- **Mateus Meneses** — [The Order: 1886 PC port](https://github.com/mateussmeneses/theorder1886-port)
  (GPL-2.0-or-later): the loader, runtime, preparation scripts, GPU layer and in-game menu this
  port starts from. Its original README is kept in
  [docs/upstream-theorder](docs/upstream-theorder/README.md).
- **Felipe Prado** — the [God of War III Remastered PC port](https://github.com/FelipePrado0/gow3_pc),
  which The Order port is copied from.
- **deadinside28** — [bloodborne_pc](https://github.com/deadinside28/bloodborne_pc), the original
  native PS4 port, and **Supermedo** (Mohammed Albarghouthi), its
  [Windows fork](https://github.com/Supermedo/bloodborne_pc).
- **cuesta4** — the [shadPS4 fork](https://github.com/cuesta4/shadPS4/tree/eltutz) (branch
  `eltutz`) whose fixes the video core carries.
- **The shadPS4 team** — the [shadPS4](https://github.com/shadps4-emu/shadPS4) emulator, whose
  video core (`gpu/shadps4/`), HLE libraries and research this port uses (GPL-2.0-or-later).

## Third-party components

| Component | Where | License |
|---|---|---|
| [shadPS4](https://github.com/shadps4-emu/shadPS4) video core | `gpu/shadps4/` | GPL-2.0-or-later |
| [sirit](https://github.com/shadps4-emu/sirit) | `gpu/third_party/sirit` | BSD-3-Clause |
| [Dear ImGui](https://github.com/ocornut/imgui) | `gpu/third_party/imgui` | MIT |
| [magic_enum](https://github.com/Neargye/magic_enum) | `gpu/third_party/magic_enum` | MIT |
| [miniz](https://github.com/richgel999/miniz) | `gpu/third_party/miniz` | MIT |
| [xbyak](https://github.com/herumi/xbyak) | `gpu/third_party/xbyak` | BSD-3-Clause |
| [FSR-Vulkan](https://github.com/FireBurn/FSR-Vulkan) / AMD FidelityFX SDK | `gpu/third_party/fsr-vulkan` | MIT |
| AMD FidelityFX FSR 1 shaders | `gpu/shadps4/video_core/host_shaders/fsr` | MIT |
| AMD GCN register headers | `gpu/third_party/gcn` | MIT (in each header) |
| [half](https://half.sourceforge.net) | `gpu/third_party/half` | MIT |
| [DejaVu fonts](https://dejavu-fonts.github.io) | `gpu/third_party/fonts` | DejaVu Fonts License ([text](gpu/third_party/fonts/LICENSE-DejaVu.txt)) |
| [Tektur](https://github.com/hyvyys/Tektur) (in-game menu font) | `gpu/third_party/fonts` | SIL Open Font License 1.1 ([text](gpu/third_party/fonts/LICENSE-Tektur-OFL.txt)) |
| FSR 4 shader set (v07 INT8, AMD FidelityFX SDK, built by [FireBurn's Q2RTX](https://github.com/FireBurn/Q2RTX)) | `fsr4_shaders` (fetched by `tools/fetch_fsr4_assets.sh`, shipped in releases) | MIT (`fsr4_shaders/LICENSE-FSR4-v07.txt`) |
| [iced-x86](https://github.com/icedland/iced) | release launcher (CPU instruction rewrite) | MIT |
| [LibAtrac9](https://github.com/Thealexbarney/LibAtrac9) | `third_party/LibAtrac9` | MIT |
| [SDL3](https://github.com/libsdl-org/SDL) | linked from MSYS2 | Zlib |
| [FFmpeg](https://ffmpeg.org) | linked from MSYS2 | LGPL-2.1-or-later |
| [Vulkan Memory Allocator](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator) | MSYS2 | MIT |
| [fmt](https://github.com/fmtlib/fmt), [Boost](https://www.boost.org), [robin-map](https://github.com/Tessil/robin-map), [xxHash](https://github.com/Cyan4973/xxHash), [Zydis](https://github.com/zyantific/zydis), [glslang](https://github.com/KhronosGroup/glslang), SPIRV-Tools / SPIRV-Cross | MSYS2 packages | MIT / BSL-1.0 / BSD / Apache-2.0 |
| [Pillow](https://python-pillow.org), [PyInstaller](https://pyinstaller.org) | launcher build | MIT-CMU / GPL-2.0 with bootloader exception |

Each component's own license file is in its folder (submodules: in the submodule).

## Game

PROTOTYPE and PROTOTYPE 2 are trademarks of their respective owners. No game code, data or art
is included in this repository or its releases; the port reads them from the player's own dump.
