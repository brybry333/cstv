#!/usr/bin/env bash
# make-linux-tar.sh: build the Linux download, dist/CSTV-linux64-<VER>.tar.gz. Needs the Source SDK headers and libmpv.
set -euo pipefail
C=$(dirname "$(readlink -f "$0")")
NAME=CSTV-linux64-${VER:-$(date +%Y%m%d)}
R=$C/dist/$NAME
make -C "$C/client" -B
make -C "$C/client" check-glibc
make -C "$C/helper" -B
rm -rf "$R"
mkdir -p "$R/client" "$R/helper" "$R/assets"
cp "$C/cstv-launch" "$C/README-linux.txt" "$C/../LICENSE" "$R/"
cp "$C/client/libcstv.so" "$R/client/"
cp "$C"/helper/{cstvd,cstv_link.py,cstv_overlay.py,cstv_ytdl.py} "$R/helper/"
cp -r "$C/helper/fonts" "$R/helper/"
cp -r "$C/assets/materials" "$C/assets/resource" "$R/assets/"
strip "$R/client/libcstv.so" "$R/helper/cstvd"
tar -C "$C/dist" -czf "$C/dist/$NAME.tar.gz" "$NAME"
du -sh "$C/dist/$NAME.tar.gz"
