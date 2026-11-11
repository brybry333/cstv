#!/usr/bin/env bash
# fetch-deps.sh — download what the Windows all-in-one zip carries, into win/deps/ (re-run to update).
# Versions are pinned below; bump them on purpose. Each download is checked against the checksum its
# project publishes where there is one.
set -euo pipefail
cd "$(dirname "$(readlink -f "$0")")"
D=deps
mkdir -p "$D"

MPV_TAG=20261017
MPV_FILE=mpv-dev-x86_64-20261017-git-dde0671072.7z      # shinchiro build: libmpv-2.dll + headers
YTDLP_TAG=2026.08.19
DENO_TAG=v2.9.7                                          # yt-dlp needs a JS runtime for YouTube
STREAMLINK_TAG=8.6.2-1
STREAMLINK_FILE=streamlink-8.6.2-1-py314-x86_64.zip      # portable, carries its own Python
PY_VER=3.14.8                                            # embeddable Python for the overlay + cstv_ytdl.py
PILLOW_VER=12.3.0

get() {   # url file
    [[ -s $D/$2 ]] && { echo "have $2"; return; }
    echo "downloading $2"
    curl -fsSL --retry 3 -o "$D/$2.part" "$1" && mv "$D/$2.part" "$D/$2"
}

get "https://github.com/shinchiro/mpv-winbuild-cmake/releases/download/$MPV_TAG/$MPV_FILE" "$MPV_FILE"
get "https://github.com/yt-dlp/yt-dlp/releases/download/$YTDLP_TAG/yt-dlp.exe" yt-dlp.exe
get "https://github.com/yt-dlp/yt-dlp/releases/download/$YTDLP_TAG/SHA2-256SUMS" yt-dlp.SHA2-256SUMS
get "https://github.com/denoland/deno/releases/download/$DENO_TAG/deno-x86_64-pc-windows-msvc.zip" deno.zip
get "https://github.com/denoland/deno/releases/download/$DENO_TAG/deno-x86_64-pc-windows-msvc.zip.sha256sum" deno.zip.sha256sum
get "https://github.com/streamlink/windows-builds/releases/download/$STREAMLINK_TAG/$STREAMLINK_FILE" "$STREAMLINK_FILE"
get "https://www.python.org/ftp/python/$PY_VER/python-$PY_VER-embed-amd64.zip" python-embed.zip

# checksums the projects publish
(cd "$D" && grep " yt-dlp.exe\$" yt-dlp.SHA2-256SUMS | sha256sum -c -)
# deno publishes PowerShell's Get-FileHash output ("Hash      : A0C3...")
(cd "$D" && echo "$(tr -d '\r' < deno.zip.sha256sum | awk '/^Hash/ {print tolower($3)}')  deno.zip" | sha256sum -c -)

# Pillow for Windows / this Python (a wheel is a zip; it gets unpacked into python\Lib\site-packages)
if ! ls "$D"/pillow-$PILLOW_VER-cp314-*win_amd64.whl >/dev/null 2>&1; then
    pip download --quiet --no-deps --only-binary=:all: --platform win_amd64 --python-version 314 \
        --implementation cp -d "$D" "pillow==$PILLOW_VER"
fi
ls -la "$D"
