#!/usr/bin/env bash
# make-zip.sh — assemble the Windows all-in-one folder win/dist/<name>/ from build/out + deps, and zip it.
# Run ./fetch-deps.sh and ./build.sh first.
set -euo pipefail
W=$(dirname "$(readlink -f "$0")")
C=${CSTV_DIR:-$HOME/.local/lib/css/cstv}
RI2=${RI2_DIR:-$HOME/Documents/ri/rawinput2-lagfix-win}
OUT=$C/win/build/out
D=$W/deps
NAME=CSTV-RawInput2-win64-${VER:-$(date +%Y%m%d)}
R=$W/dist/$NAME
rm -rf "$R"
mkdir -p "$R/cstv/fonts" "$R/LICENSES"

cp "$OUT/RawInput2BunnyhopAPE.exe" "$OUT/cstv_client.dll" "$R/"
cp "$OUT/cstvd.exe" "$OUT/cstv_ytdl.exe" "$C/helper/cstv_overlay.py" "$C/helper/cstv_ytdl.py" "$R/cstv/"
cp "$C/win/build/mpv-dev/libmpv-2.dll" "$D/yt-dlp.exe" "$R/cstv/"
unzip -q -o "$D/deno.zip" -d "$R/cstv/"

# embeddable Python + Pillow (a wheel is a zip); the ._pth file lists where imports come from
unzip -q "$D/python-embed.zip" -d "$R/cstv/python"
mkdir -p "$R/cstv/python/Lib/site-packages"
unzip -q "$D"/pillow-*-win_amd64.whl -d "$R/cstv/python/Lib/site-packages"
pth=$(ls "$R/cstv/python"/python3*._pth)
printf 'Lib\\site-packages\r\n' >> "$pth"

# portable streamlink without its 145 MB ffmpeg.exe (only needed to mux separate audio/video streams;
# Twitch and YouTube live are single HLS streams)
unzip -q "$D"/streamlink-*-x86_64.zip -d "$R/cstv/"
mv "$R/cstv"/streamlink-*-x86_64 "$R/cstv/streamlink"
rm -rf "$R/cstv/streamlink/ffmpeg"

# the screen's placeholder material: the launcher copies it into cstrike\custom\cstv before the game starts
mkdir -p "$R/cstv/materials/cstv"
cp "$C"/assets/materials/cstv/{screen,chat}.{vmt,vtf} "$R/cstv/materials/cstv/"

cp /usr/share/fonts/noto/NotoSans-Regular.ttf /usr/share/fonts/noto/NotoSans-Bold.ttf \
   /usr/share/fonts/noto/NotoColorEmoji.ttf "$R/cstv/fonts/"

cp "$W/README-windows.txt" "$R/README.txt"
cp "$RI2/LICENSE" "$R/LICENSES/GPL-3.0.txt"
cp /usr/share/licenses/noto-fonts/LICENSE "$R/LICENSES/Noto-fonts-OFL.txt"
cp "$RI2/RawInput2/Detours/LICENSE.md" "$R/LICENSES/Detours-MIT.md"
cat > "$R/LICENSES/SOURCES.txt" <<SRC
Where the parts come from (and their source code):
  RawInput2BunnyhopAPE.exe  GPL-3.0  github.com/bhopbhopbhop/RawInput2BunnyhopAPE-lagfix (+ local changes:
                            trigger prediction fix, CS:TV launcher support), uses Microsoft Detours (MIT)
  cstv_client.dll, cstv\cstvd.exe, cstv\*.py, cstv_ytdl.exe
                            CS:TV; source available from whoever gave you this. cstvd.exe links libmpv,
                            so it is distributed under the GPL (GPL-3.0.txt).
  cstv\libmpv-2.dll         mpv (GPL build) github.com/mpv-player/mpv, built by
                            github.com/shinchiro/mpv-winbuild-cmake (release $(basename "$D"/mpv-dev-*.7z .7z))
  cstv\yt-dlp.exe           Unlicense  github.com/yt-dlp/yt-dlp
  cstv\deno.exe             MIT        github.com/denoland/deno
  cstv\streamlink\          BSD-2      github.com/streamlink/windows-builds (its own licenses inside)
  cstv\python\              PSF        python.org (LICENSE.txt inside); Pillow (MIT-CMU, in site-packages)
  cstv\fonts\               SIL OFL    Noto fonts (Noto-fonts-OFL.txt)
SRC
(cd "$W/dist" && rm -f "$NAME.zip" && 7z a -tzip -mx=7 -bso0 -bsp0 "$NAME.zip" "$NAME")
du -sh "$R" "$W/dist/$NAME.zip"
