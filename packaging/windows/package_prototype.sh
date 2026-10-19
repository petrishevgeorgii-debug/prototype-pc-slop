#!/usr/bin/env bash
# Builds the PROTOTYPE port release: dist/PROTOTYPE-PC-Port-<version>/ and its .zip.
# Run from an MSYS2 CLANG64 shell in the port folder after `bash build.sh` (setup.bat does both).
# The package holds the port only: Prototype.exe (the launcher, frozen with PyInstaller; its icon
# is the port's own, not the game's), theorder-probe.exe with the CLANG64 DLLs it loads, run.py,
# the preparation scripts, the patches and the FSR 4 shader set (MIT, AMD FidelityFX SDK).
# No game file and nothing made from one (the prepared image in out/, shader caches, saves,
# settings, logs, the game icon) is copied; the script stops if it finds one.
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
source ./msys2-env.sh
version=$(sed -n "s/^VERSION = '\(.*\)'/\1/p" launcher/prototype_launcher.py)
name="PROTOTYPE-PC-Port-v${version}"
[[ -f out/theorder-probe.exe && -f out/theorder-gpu-capabilities.exe ]] || { echo 'Build first: bash build.sh' >&2; exit 1; }
[[ -x out/pyenv/Scripts/python.exe ]] || { echo 'Run setup.bat first (it makes out\pyenv)' >&2; exit 1; }
[[ -f fsr4_shaders/LICENSE-FSR4-v07.txt ]] || bash tools/fetch_fsr4_assets.sh

echo "Packaging $name ..."
out/pyenv/Scripts/python.exe -m pip install -q --disable-pip-version-check pyinstaller pillow iced-x86==1.21.0
out/pyenv/Scripts/python.exe -m PyInstaller --noconfirm --clean --log-level WARN --onefile --windowed \
    --name Prototype --icon "$(cygpath -w "$PWD/launcher/prototype_port.ico")" \
    --distpath out/rel-pyi-dist --workpath out/rel-pyi-work --specpath out/rel-pyi-work \
    --paths "$(cygpath -w "$PWD/scripts")" --hidden-import iced_x86 \
    "$(cygpath -w "$PWD/launcher/prototype_launcher.py")"

dest=out/rel-stage/$name
rm -rf -- out/rel-stage
mkdir -p "$dest/bin"
cp out/rel-pyi-dist/Prototype.exe "$dest/"
for exe in theorder-probe.exe theorder-gpu-capabilities.exe; do
    llvm-strip --strip-debug -o "$dest/bin/$exe" "out/$exe"
done
# The librsvg stub first: ldd then finds it beside the executable and GIO stays out.
cp out/librsvg-2-2.dll "$dest/bin/"
ldd "$dest/bin/theorder-probe.exe" "$dest/bin/theorder-gpu-capabilities.exe" |
    awk '/\/clang64\/bin\// {print $3}' | sort -u | while read -r dll; do
        cp -u "$dll" "$dest/bin/"
    done
cp -r scripts patches "$dest/"
mkdir -p "$dest/fsr4_shaders"
cp -r fsr4_shaders/. "$dest/fsr4_shaders/"
mkdir -p "$dest/licenses"
cp gpu/third_party/fonts/LICENSE-Tektur-OFL.txt gpu/third_party/fonts/LICENSE-DejaVu.txt "$dest/licenses/"
# The key caps of the keyboard prompts (scripts/proto_prompts.py) use the in-game menu's font.
mkdir -p "$dest/fonts"
cp gpu/third_party/fonts/Tektur-SemiCondensed.ttf gpu/third_party/fonts/LICENSE-Tektur-OFL.txt "$dest/fonts/"
cp run.py LICENSE README.md CREDITS.md "$dest/"
cp packaging/windows/README-RELEASE.txt "$dest/READ ME FIRST.txt"
find "$dest" -name __pycache__ -prune -exec rm -r {} +

# Nothing from the game or from a player may ship.
forbidden=$(find "$dest" \( -iname eboot.bin -o -iname eboot.elf -o -iname 'boot*.bin' -o -iname entry.bin \
    -o -iname sce_module -o -iname sce_sys -o -iname savedata -o -iname user -o -iname cache \
    -o -iname '*.log' -o -iname theorder.ini -o -iname 'link.json' -o -iname 'libc-link.json' \
    -o -iname analysis.json -o -iname 'content*.bin' -o -iname patches.bin -o -iname prototype.ico \
    -o -iname window_icon.bmp -o -iname '*.pkg' -o -iname '*.sprx' -o -iname '*.prx' -o -iname '*.rdc' \
    -o -iname dump -o -iname '*.raw' -o -iname 'nvngx_dlss*.dll' -o -iname prompts -o -iname '*.p3d*' \
    -o -iname '*.gfx' \) -print)
if [[ -n $forbidden ]]; then
    echo "Package holds files that must not ship:" >&2
    echo "$forbidden" >&2
    exit 1
fi
mkdir -p dist
rm -rf "dist/$name" "dist/$name.zip"
cp -r "$dest" "dist/"
(cd out/rel-stage && powershell -NoProfile -Command \
    "Compress-Archive -Path '$name' -DestinationPath '../../dist/$name.zip'")
echo "Release: dist/$name.zip ($(du -h "dist/$name.zip" | cut -f1))"
