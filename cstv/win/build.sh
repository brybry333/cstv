#!/usr/bin/env bash
# build.sh: cross-compile the Windows parts on Linux into win/build/out/:
#   CSTV.exe, cstvd.exe, cstv_ytdl.exe   MinGW-w64
#   cstv_client.dll                      clang-cl + xwin's MSVC CRT/SDK (MSVC ABI: it implements game interfaces)
# Needs: clang, llvm, lld, mingw-w64-gcc, an xwin splat in ~/.local/share/xwin (or XWIN=), ./fetch-deps.sh run,
# and the Source SDK headers in third_party/source-sdk-2013 (see the README). No spaces in this folder's path.
set -euo pipefail
W=$(dirname "$(readlink -f "$0")")
C=$(dirname "$W")
X=${XWIN:-$HOME/.local/share/xwin}
B=$C/win/build
OUT=$B/out
mkdir -p "$OUT" "$B/client" "$B/mpv-dev"
[[ -f $B/mpv-dev/libmpv.dll.a ]] || (cd "$B/mpv-dev" && 7z x -y -bso0 -bsp0 "$W"/deps/mpv-dev-x86_64-*.7z)

echo "== CSTV.exe, cstvd.exe, cstv_ytdl.exe"
x86_64-w64-mingw32-gcc -O2 -Wall -municode -mwindows -o "$OUT/CSTV.exe" "$W/launcher.c" -static -ladvapi32 -lshell32 -luser32
# x86-64-v2 = any 64-bit CPU since ~2009; the encoder picks its AVX2 copy at runtime
x86_64-w64-mingw32-gcc -O3 -march=x86-64-v2 -ffile-prefix-map="$C"=. -Wall -Wextra -Wno-unused-parameter -D_WIN32_WINNT=0x0A00 \
    -I"$C/common" -I"$C/third_party/stb" -I"$B/mpv-dev/include" -o "$OUT/cstvd.exe" "$C/helper/cstvd.c" \
    "$B/mpv-dev/libmpv.dll.a" -lws2_32 -static -lpthread -lm 2>&1 | grep -v stb_truetype || true
x86_64-w64-mingw32-gcc -O2 -Wall -municode -o "$OUT/cstv_ytdl.exe" "$C/helper/cstv_ytdl_win.c" -static

echo "== cstv_client.dll"
SDK=$C/third_party/source-sdk-2013/src
MSVC="--target=x86_64-pc-windows-msvc -nologo -imsvc $X/crt/include -imsvc $X/sdk/include/ucrt -imsvc $X/sdk/include/um -imsvc $X/sdk/include/shared"
LIBS="-libpath:$X/crt/lib/x86_64 -libpath:$X/sdk/lib/um/x86_64 -libpath:$X/sdk/lib/ucrt/x86_64"
# win/sdk-compat: two SDK headers patched for clang-cl, force-included so the originals are skipped
CF="$MSVC /clang:-ffile-prefix-map=$C=. -O2 -MT -EHs-c- -GR- -Wno-everything -D_WIN32 -DWIN32 -D_WINDOWS -D_WIN64 -DCOMPILER_MSVC -DCOMPILER_MSVC64 \
    -DCLIENT_DLL -D_CRT_SECURE_NO_WARNINGS -DNOMINMAX -I$C/win/sdk-compat -I$SDK/public -I$SDK/public/tier0 \
    -I$SDK/public/tier1 -I$SDK/common -I$C/common -I$C/client -I$C/third_party/stb \
    -FI$C/win/sdk-compat/tier1/bitbuf.h -FI$C/win/sdk-compat/mathlib/ssemath.h"
for f in cstv_client cstv_gui cstv_platform_win; do
    clang-cl $CF -c "$C/client/$f.cpp" -Fo"$B/client/$f.obj"
done
lld-link -nologo -dll -out:"$OUT/cstv_client.dll" $LIBS "$B"/client/{cstv_client,cstv_gui,cstv_platform_win}.obj \
    libcmt.lib libvcruntime.lib libucrt.lib kernel32.lib user32.lib ws2_32.lib
ls -la "$OUT"
