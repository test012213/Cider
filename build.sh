#!/usr/bin/env bash
# Builds Cider with LLVM-MinGW (clang targeting x86_64-w64-mingw32).
#   ./build.sh          build everything into build/
#   ./build.sh tests    also build the macOS test programs (needs ld64.lld)
set -euo pipefail
cd "$(dirname "$0")"

CC=${CC:-clang}
WINDRES=${WINDRES:-windres}
OUT=build
mkdir -p "$OUT/obj"

CFLAGS="-O2 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Wno-sign-compare -Wno-cast-function-type -municode"
LIBS="-lgdiplus -lgdi32 -lcomctl32 -lshell32 -lole32 -luuid -lurlmon -ldwmapi -lcomdlg32 -ladvapi32 -lshlwapi"

echo "== icons"
"$CC" -O2 -o "$OUT/make_icon.exe" tools/make_icon.c
"./$OUT/make_icon.exe" assets

echo "== runtime"
RT_SRC="util plist macho libsystem dmg backends ui"
objs=()
for f in $RT_SRC; do
  "$CC" $CFLAGS -c "src/cider/$f.c" -o "$OUT/obj/$f.o"
  objs+=("$OUT/obj/$f.o")
done
"$CC" $CFLAGS -c src/common/gfx.c -o "$OUT/obj/gfx.o"
"$CC" $CFLAGS -c src/cider/thunks.S -o "$OUT/obj/thunks.o"
(cd src/cider && "$WINDRES" -I. -i cider.rc -O coff -o "../../$OUT/obj/cider.res.o")
objs+=("$OUT/obj/gfx.o" "$OUT/obj/thunks.o" "$OUT/obj/cider.res.o")

"$CC" $CFLAGS -c src/cider/main.c -o "$OUT/obj/main_con.o"
"$CC" $CFLAGS -DCIDER_GUI -c src/cider/main.c -o "$OUT/obj/main_gui.o"
"$CC" $CFLAGS -DCIDER_GUI -c src/cider/util.c -o "$OUT/obj/util_gui.o"

gui_objs=("${objs[@]/$OUT\/obj\/util.o/$OUT/obj/util_gui.o}")
"$CC" $CFLAGS -o "$OUT/cider.exe" "$OUT/obj/main_con.o" "${objs[@]}" $LIBS
"$CC" $CFLAGS -mwindows -o "$OUT/ciderw.exe" "$OUT/obj/main_gui.o" "${gui_objs[@]}" $LIBS

echo "== installer"
cp "$OUT/cider.exe" "$OUT/ciderw.exe" src/setup/ 2>/dev/null || true
(cd src/setup && "$WINDRES" -I. -I../cider -i setup.rc -O coff -o "../../$OUT/obj/setup.res.o")
"$CC" $CFLAGS -c src/setup/setup.c -o "$OUT/obj/setup.o"
"$CC" $CFLAGS -mwindows -o "$OUT/CiderSetup.exe" "$OUT/obj/setup.o" "$OUT/obj/gfx.o" "$OUT/obj/setup.res.o" $LIBS
rm -f src/setup/cider.exe src/setup/ciderw.exe

if [ "${1:-}" = "tests" ]; then
  echo "== macOS test programs"
  ./tests/build_tests.sh
fi

echo
ls -la "$OUT"/*.exe
