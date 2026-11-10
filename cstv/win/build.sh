#!/usr/bin/env bash
# build.sh — cross-compile the Windows parts on Linux into win/build/out/:
#   cstvd.exe, cstv_ytdl.exe   MinGW-w64 (C, links libmpv-2.dll)
#   cstv_client.dll            clang-cl + xwin's MSVC CRT/SDK (MSVC ABI: it implements game interfaces)
#   RawInput2BunnyhopAPE.exe   clang-cl, from the lagfix fork checkout (RI2_DIR), with Detours
# Needs: clang, llvm, lld, mingw-w64-gcc (pacman), xwin splat in ~/.local/share/xwin, ./fetch-deps.sh run.
set -euo pipefail
W=$(dirname "$(readlink -f "$0")")
# make/clang-cl can't take the spaces in "counter strike source shit": use the space-free alias
C=${CSTV_DIR:-$HOME/.local/lib/css/cstv}
RI2=${RI2_DIR:-$HOME/Documents/ri/rawinput2-lagfix-win/RawInput2}
X=${XWIN:-$HOME/.local/share/xwin}
B=$C/win/build
OUT=$B/out
mkdir -p "$OUT" "$B/client" "$B/launcher" "$B/mpv-dev"
[[ -f $B/mpv-dev/libmpv.dll.a ]] || (cd "$B/mpv-dev" && 7z x -y -bso0 -bsp0 "$W"/deps/mpv-dev-x86_64-*.7z)

MSVC="--target=x86_64-pc-windows-msvc -nologo -imsvc $X/crt/include -imsvc $X/sdk/include/ucrt -imsvc $X/sdk/include/um -imsvc $X/sdk/include/shared"
LIBS="-libpath:$X/crt/lib/x86_64 -libpath:$X/sdk/lib/um/x86_64 -libpath:$X/sdk/lib/ucrt/x86_64"

echo "== cstvd.exe, cstv_ytdl.exe"
# x86-64-v2 = any 64-bit CPU since ~2009; the encoder picks its AVX2 copy at runtime
x86_64-w64-mingw32-gcc -O3 -march=x86-64-v2 -g -Wall -Wextra -Wno-unused-parameter -D_WIN32_WINNT=0x0A00 \
    -I"$C/common" -I"$C/third_party/stb" -I"$B/mpv-dev/include" -o "$OUT/cstvd.exe" "$C/helper/cstvd.c" \
    "$B/mpv-dev/libmpv.dll.a" -lws2_32 -static -lpthread -lm 2>&1 | grep -v stb_truetype || true
x86_64-w64-mingw32-gcc -O2 -Wall -municode -o "$OUT/cstv_ytdl.exe" "$C/helper/cstv_ytdl_win.c" -static

echo "== cstv_client.dll"
SDK=$C/third_party/source-sdk-2013/src
# win/sdk-compat: two SDK headers patched for clang-cl, force-included so the originals are skipped
CF="$MSVC -O2 -MT -EHs-c- -GR- -Wno-everything -D_WIN32 -DWIN32 -D_WINDOWS -D_WIN64 -DCOMPILER_MSVC -DCOMPILER_MSVC64 \
    -DCLIENT_DLL -D_CRT_SECURE_NO_WARNINGS -DNOMINMAX -I$C/win/sdk-compat -I$SDK/public -I$SDK/public/tier0 \
    -I$SDK/public/tier1 -I$SDK/common -I$C/common -I$C/client \
    -FI$C/win/sdk-compat/tier1/bitbuf.h -FI$C/win/sdk-compat/mathlib/ssemath.h"
clang-cl $CF -c "$C/client/cstv_client.cpp" -Fo"$B/client/cstv_client.obj"
clang-cl $CF -c "$C/client/cstv_platform_win.cpp" -Fo"$B/client/cstv_platform_win.obj"
lld-link -nologo -dll -out:"$OUT/cstv_client.dll" $LIBS "$B/client/cstv_client.obj" "$B/client/cstv_platform_win.obj" \
    libcmt.lib libvcruntime.lib libucrt.lib kernel32.lib ws2_32.lib

echo "== RawInput2BunnyhopAPE.exe"
# /MD like the upstream project: its self-injection expects the VC runtime DLLs at the same address
# in the game. /Zc:threadSafeInit-: no TLS-based static guards in code that runs copied into the game
# (a copied image has no TLS of its own there).
LF="$MSVC -O2 -MD -EHsc -Zc:threadSafeInit- -Wno-everything -DNDEBUG -D_CONSOLE -D_MBCS"
for f in Detours/src/detours.cpp Detours/src/disasm.cpp Detours/src/modules.cpp main.cpp teleport_predict.cpp; do
    clang-cl $LF -c "$RI2/$f" -Fo"$B/launcher/$(basename "$f" .cpp).obj"
done
lld-link -nologo -out:"$OUT/RawInput2BunnyhopAPE.exe" -subsystem:console -opt:ref -opt:icf $LIBS \
    "$B"/launcher/{detours,disasm,modules,main,teleport_predict}.obj \
    msvcrt.lib vcruntime.lib ucrt.lib msvcprt.lib kernel32.lib user32.lib advapi32.lib urlmon.lib wininet.lib shell32.lib
ls -la "$OUT"
