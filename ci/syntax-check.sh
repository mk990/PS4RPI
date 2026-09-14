#!/bin/sh
#
# Parses every first-party source file with a host clang against the stub SDK
# headers in ci/stubs, with the same warning set the real build uses.
#
# This is a lint, not a build: it cannot link and it knows nothing about the
# real SDK's struct layouts. What it does catch is the class of mistake that
# bit this project before -- a format string that does not match its argument,
# a label nothing jumps to, a call whose signature drifted.
#
# Vendored single-header libraries are skipped; they are upstream code.

set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
src="$root/RPI"
stubs="$root/ci/stubs"

CC=${CC:-clang}

# Upstream code we do not police.
vendored='sandbird.c tiny-json.c'

# module.c and installer.c are thin wrappers over SDK calls whose struct
# layouts would have to be mirrored exactly; a wrong guess here would fail CI
# for a source file that is actually fine. They are covered by the real build.
skip="$vendored module.c installer.c"

warnings="-Wall -Wextra
  -Wno-unused-parameter
  -Wno-sign-compare
  -Wno-missing-field-initializers
  -Wno-implicit-function-declaration
  -Wno-incompatible-library-redeclaration
  -Wno-deprecated-non-prototype"

status=0

for path in "$src"/*.c; do
  file=$(basename "$path")

  case " $skip " in
    *" $file "*) printf '%-14s skipped\n' "$file"; continue ;;
  esac

  if out=$($CC -fsyntax-only -std=gnu99 -I"$src" -I"$stubs" $warnings "$path" 2>&1) && [ -z "$out" ]; then
    printf '%-14s ok\n' "$file"
  else
    printf '%-14s FAILED\n' "$file"
    printf '%s\n' "$out"
    status=1
  fi
done

exit $status
