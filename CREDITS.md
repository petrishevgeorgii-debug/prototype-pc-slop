# Credits

## This port

- **petrishevgeorgii-debug** — PROTOTYPE port: testing, direction, releases.
- Port work (PROTOTYPE runtime additions, fixes, patches, FSR integration, in-game menu,
  launcher) was written with **Claude** (Anthropic).

## The ports and projects this one is built on

This project is a fork of, and would not exist without:

- **Mateus Meneses** — [The Order: 1886 PC port](https://github.com/mateussmeneses/theorder1886-port)
  (GPL-2.0-or-later): the loader, runtime, preparation scripts, GPU layer, temporal upscaler
  integration and in-game menu this port starts from. Its full commit history is kept in this
  repository and its original README in [docs/upstream-theorder](docs/upstream-theorder/README.md).
- **Felipe Prado** — the [God of War III Remastered PC port](https://github.com/FelipePrado0/gow3_pc),
  which The Order port is copied from.
- **deadinside28** — [bloodborne_pc](https://github.com/deadinside28/bloodborne_pc), the original
  native PS4 port, and **Supermedo** (Mohammed Albarghouthi), its
  [Windows fork](https://github.com/Supermedo/bloodborne_pc).
- **cuesta4** — the [shadPS4 fork](https://github.com/cuesta4/shadPS4/tree/eltutz) (branch
  `eltutz`) whose fixes the video core carries.
- **The shadPS4 team and contributors** — the [shadPS4](https://github.com/shadps4-emu/shadPS4)
  emulator, whose video core (`gpu/shadps4/`), shader recompiler, HLE research and function name
  database (aerolib, used for `src/import_names.inc`) this port uses (GPL-2.0-or-later).
- **AMD** — FidelityFX Super Resolution 1, 3.1 and 4 (FidelityFX SDK, MIT), and **FireBurn** —
  [FSR-Vulkan](https://github.com/FireBurn/FSR-Vulkan) and the FSR 4 Vulkan shader set built in
  [Q2RTX](https://github.com/FireBurn/Q2RTX).

## Third-party components in the source

| Component | Where | License |
|---|---|---|
| [shadPS4](https://github.com/shadps4-emu/shadPS4) video core | `gpu/shadps4/` | GPL-2.0-or-later |
| [sirit](https://github.com/shadps4-emu/sirit) | `gpu/third_party/sirit` | BSD-3-Clause |
| [Dear ImGui](https://github.com/ocornut/imgui) (Omar Cornut) | `gpu/third_party/imgui` | MIT |
| [magic_enum](https://github.com/Neargye/magic_enum) | `gpu/third_party/magic_enum` | MIT |
| [miniz](https://github.com/richgel999/miniz) | `gpu/third_party/miniz` | MIT |
| [xbyak](https://github.com/herumi/xbyak) | `gpu/third_party/xbyak` | BSD-3-Clause |
| [FSR-Vulkan](https://github.com/FireBurn/FSR-Vulkan) / AMD FidelityFX SDK | `gpu/third_party/fsr-vulkan` | MIT |
| AMD FidelityFX FSR 1 shaders | `gpu/shadps4/video_core/host_shaders/fsr` | MIT |
| AMD GCN register headers | `gpu/third_party/gcn` | MIT (in each header) |
| [half](https://half.sourceforge.net) (Christian Rau) | `gpu/third_party/half` | MIT |
| [DejaVu fonts](https://dejavu-fonts.github.io) | `gpu/third_party/fonts` | DejaVu Fonts License ([text](gpu/third_party/fonts/LICENSE-DejaVu.txt)) |
| [Tektur](https://github.com/hyvyys/Tektur) (Adam Jagosz), in-game menu font | `gpu/third_party/fonts` | SIL Open Font License 1.1 ([text](gpu/third_party/fonts/LICENSE-Tektur-OFL.txt)) |
| [LibAtrac9](https://github.com/Thealexbarney/LibAtrac9) (Alex Barney) | `third_party/LibAtrac9` | MIT |

Each component's own license file is in its folder (submodules: in the submodule).

## Third-party components in the release package

| Component | Where in the package | License |
|---|---|---|
| FSR 4 shader set (v07 INT8; AMD FidelityFX SDK, built by FireBurn's Q2RTX) | `fsr4_shaders` | MIT (`fsr4_shaders/LICENSE-FSR4-v07.txt`) |
| [Tektur](https://github.com/hyvyys/Tektur) font (Adam Jagosz): the keyboard prompts' key caps | `fonts` | SIL Open Font License 1.1 (`fonts/LICENSE-Tektur-OFL.txt`) |
| [SDL3](https://github.com/libsdl-org/SDL) | `bin/SDL3.dll` | Zlib |
| [FFmpeg](https://ffmpeg.org) and the codec libraries of its MSYS2 build (x264, x265, libvpx, dav1d, libaom, rav1e, SVT-AV1, opus, vorbis, …) | `bin/av*.dll`, `bin/sw*.dll`, `bin/lib*.dll` | LGPL-2.1-or-later / GPL-2.0-or-later (x264, x265) / BSD / MIT, per library |
| [LLVM libc++](https://libcxx.llvm.org), libomp, libwinpthread | `bin/` | Apache-2.0 WITH LLVM-exception / MIT / ZPL |
| [Zydis](https://github.com/zyantific/zydis), [xxHash](https://github.com/Cyan4973/xxHash), zlib, zstd | `bin/` | MIT / BSD-2-Clause / Zlib / BSD |
| [Python](https://www.python.org), [PyInstaller](https://pyinstaller.org) (bootloader), [Pillow](https://python-pillow.org), [iced-x86](https://github.com/icedland/iced) | `Prototype.exe` | PSF-2.0 / GPL-2.0 with bootloader exception / MIT-CMU / MIT |
| Build toolchain: [MSYS2](https://www.msys2.org) CLANG64, [LLVM](https://llvm.org), [glslang](https://github.com/KhronosGroup/glslang), SPIRV-Tools / SPIRV-Cross, [Boost](https://www.boost.org), [fmt](https://github.com/fmtlib/fmt), [robin-map](https://github.com/Tessil/robin-map), [Vulkan Memory Allocator](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator) | build only | various open-source licenses |

The DLLs in `bin/` are unmodified MSYS2 CLANG64 packages; their sources are available from
[MSYS2](https://packages.msys2.org) (`mingw-w64-clang-x86_64-*`). The port's own source is this
repository (GPL-2.0-or-later); each release is built from the tagged commit.

## Game

PROTOTYPE and PROTOTYPE 2 are trademarks of their respective owners (developed by Radical
Entertainment, published by Activision). No game code, data or art is included in this
repository or its releases; the port reads them from the player's own dump. This is an
unofficial fan project, not affiliated with or endorsed by Activision, Radical Entertainment or
Sony Interactive Entertainment.
