#!/bin/sh
# Build expat (XML parser) as a static libexpat.a for the b1nix userspace ABI.
# Dependency of Fontconfig. Freestanding compile of the 3 core sources. Prints
# the install dir.
#
# Build logic lives in tools/ports/drivers/cport.sh; this is the manifest.
set -eu
ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"

CPORT_NAME=expat
CPORT_ARCHIVE=libexpat.a
VER="${EXPAT_VERSION:-2.6.2}"
CPORT_SRCNAME="expat-${VER}"
CPORT_TARBALL="expat-${VER}.tar.xz"
CPORT_URL="https://github.com/libexpat/libexpat/releases/download/R_2_6_2/expat-${VER}.tar.xz"
CPORT_SOURCES="lib/xmlparse.c lib/xmlrole.c lib/xmltok.c"
CPORT_HEADERS="flat:lib/expat.h flat:lib/expat_external.h"

# XML_POOR_ENTROPY: b1nix has no getrandom/arc4random; expat falls back to a weak
# hash salt (fine for parsing trusted local config). XML_GE=1 enables general
# entities (Fontconfig uses them).
port_pre_build() {
  cat > "$GEN_DIR/expat_config.h" <<EOF
#define XML_GE 1
#define XML_DTD 1
#define XML_NS 1
#define XML_CONTEXT_BYTES 1024
#define BYTEORDER 1234
#define HAVE_MEMMOVE 1
#define XML_POOR_ENTROPY 1
#define PACKAGE_VERSION "$VER"
EOF
  CPORT_CFLAGS="-DXML_GE=1 -DXML_DTD -DXML_NS -DBYTEORDER=1234 -DXML_POOR_ENTROPY \
-DHAVE_MEMMOVE=1 -DHAVE_EXPAT_CONFIG_H=1 -I$GEN_DIR -I$SRC_DIR/lib \
-Wno-implicit-function-declaration -Wno-incompatible-pointer-types -Wno-int-conversion \
-Wno-implicit-int -Wno-incompatible-function-pointer-types"
}

. "$ROOT_DIR/tools/ports/drivers/cport.sh"
