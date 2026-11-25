#!/usr/bin/env bash
# Cross-compiles the test programs into real macOS x86_64 Mach-O binaries,
# without a Mac or Apple's SDK: clang targets x86_64-apple-macos and lld's
# ld64 flavour links against a generated libSystem.tbd stub.
#
# Two flavours are produced, because real-world apps use both:
#   classic/  macOS 11 target: LC_DYLD_INFO rebase/bind opcodes
#   chained/  macOS 13 target: LC_DYLD_CHAINED_FIXUPS
set -euo pipefail
cd "$(dirname "$0")/.."

OUT=build/tests
mkdir -p "$OUT/classic" "$OUT/chained" "$OUT/obj"

LLD="$(dirname "$(command -v clang)")/ld.lld.exe"
LD64="$OUT/ld64.lld.exe"          # lld picks its Mach-O flavour from argv[0]
[ -f "$LD64" ] || cp "$LLD" "$LD64"

# libSystem stub listing every symbol Cider implements
{
  echo "--- !tapi-tbd"
  echo "tbd-version: 4"
  echo "targets: [ x86_64-macos ]"
  echo "install-name: '/usr/lib/libSystem.B.dylib'"
  echo "current-version: 1319"
  echo "exports:"
  echo "  - targets: [ x86_64-macos ]"
  echo "    symbols: ["
  { grep -oE '\bS\([a-zA-Z0-9_]+\)' src/cider/libsystem.c | sed -E 's/S\((.*)\)/_\1/'
    grep -oE 'SA\("[^"]+"' src/cider/libsystem.c | sed -E 's/SA\("(.*)"/\1/'
    printf '%s\n' '_fopen$DARWIN_EXTSN' '_stat$INODE64' '_opendir$INODE64' '_readdir$INODE64'
  } | sort -u | sed -E "s/.*/      '&',/"
  echo "      ]"
  echo "..."
} > "$OUT/libSystem.tbd"

CFLAGS="-target x86_64-apple-macos11 -O2 -fno-builtin -ffreestanding -Wno-incompatible-library-redeclaration"

for src in tests/src/t_*.c tests/src/libgreet.c; do
  name=$(basename "$src" .c)
  clang $CFLAGS -c "$src" -o "$OUT/obj/$name.o"
done

link() { # flavour min-version output inputs... [extra flags]
  local flavour=$1 ver=$2 out=$3; shift 3
  "$LD64" -arch x86_64 -platform_version macos "$ver" "$ver" -o "$OUT/$flavour/$out" "$@" "$OUT/libSystem.tbd"
}

for flavour in classic chained; do
  ver=11.0; [ $flavour = chained ] && ver=13.0
  extra=(); [ $flavour = chained ] && extra=(-fixup_chains)
  for t in t_printf t_libc t_files t_threads; do
    link $flavour $ver $t "$OUT/obj/$t.o" "${extra[@]}"
  done
  link $flavour $ver libgreet.dylib "$OUT/obj/libgreet.o" -dylib -install_name @rpath/libgreet.dylib "${extra[@]}"
  link $flavour $ver t_dylib "$OUT/obj/t_dylib.o" "$OUT/$flavour/libgreet.dylib" -rpath @executable_path "${extra[@]}"
  link $flavour $ver t_missing "$OUT/obj/t_missing.o" -undefined dynamic_lookup "${extra[@]}"
done

# An Apple Silicon-only binary, to check Cider explains why it can't run it.
clang -target arm64-apple-macos11 -O2 -fno-builtin -ffreestanding -c tests/src/t_printf.c -o "$OUT/obj/t_printf_arm64.o"
sed 's/x86_64-macos/arm64-macos/g' "$OUT/libSystem.tbd" > "$OUT/libSystem_arm64.tbd"
"$LD64" -arch arm64 -platform_version macos 11.0 11.0 -o "$OUT/t_arm64_only" "$OUT/obj/t_printf_arm64.o" "$OUT/libSystem_arm64.tbd"

# A universal (fat) binary: arm64 + x86_64, like most apps on the Mac App Store.
python -I tests/tools/lipo.py "$OUT/t_universal" "$OUT/t_arm64_only" "$OUT/classic/t_printf"

# App bundles and a disk image built from the binaries above.
python -I tests/tools/make_fixtures.py "$OUT/fixtures" "$OUT"

echo "test binaries built in $OUT"
