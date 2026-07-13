#!/bin/sh
# Build and install the Purple Perl XS module via its Makefile.PL.
# Args: <perl> <common-dir>
set -e
PERL="$1"
COMMON_DIR="$2"

if [ -z "$MESON_INSTALL_DESTDIR_PREFIX" ]; then
    PREFIX="${MESON_INSTALL_PREFIX:-/usr/local}"
else
    PREFIX="$MESON_INSTALL_DESTDIR_PREFIX"
fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
cp -r "$COMMON_DIR"/. "$WORK"/
cd "$WORK"

# Generate the Makefile then build+install the module.
"$PERL" Makefile.PL PREFIX="$PREFIX" 2>/dev/null || {
    echo "pidgin: skipping Perl XS module (Makefile.PL failed)" >&2
    exit 0
}
make 2>/dev/null || {
    echo "pidgin: skipping Perl XS module (make failed)" >&2
    exit 0
}
make install 2>/dev/null || {
    echo "pidgin: skipping Perl XS module (install failed)" >&2
    exit 0
}
echo "pidgin: installed Purple Perl XS module"
