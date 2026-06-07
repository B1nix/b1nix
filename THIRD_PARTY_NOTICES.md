# Third-Party Software

The GPL and LGPL licenses described in `LICENSING.md` apply only to original
B1NIX material for which the B1NIX copyright holder has the right to grant
those licenses. They do not replace or override third-party licenses.

## Source Included in This Repository

### TinyCC

- Location: `userspace/tcc/`
- Version: 0.9.28rc
- License: GNU Lesser General Public License, version 2.1 or later
- License text: `userspace/tcc/COPYING`
- Upstream: <https://repo.or.cz/tinycc.git>

TinyCC retains its upstream copyright and license notices. Modifications to
TinyCC must remain under its applicable license and must be identified as
required by that license.

## Software Downloaded During the Build

Depending on the build target and configuration, B1NIX downloads, builds,
statically links, or embeds the following software:

| Component | Default version | License summary |
| --- | ---: | --- |
| BusyBox | 1.36.1 | GNU GPL version 2 |
| GNU Wget | 1.21.4 | GNU GPL version 3 or later |
| curl | 8.20.0 | curl license |
| Dropbear | 2022.83 | Dropbear license; bundled libraries have their own notices |
| Mbed TLS | 3.6.0 | Apache-2.0 OR GPL-2.0-or-later |
| PCRE2 | 10.44 | BSD-style PCRE2 license |
| OpenSSL | 1.1.1w | OpenSSL and original SSLeay licenses |
| GNU libidn2 | 2.3.7 | GNU LGPL-3.0-or-later OR GPL-2.0-or-later, with separately licensed data |
| GNU libunistring | 1.2 | GNU LGPL-3.0-or-later OR GPL-2.0-or-later |
| libpsl | 0.21.5 | MIT license, with separately licensed bundled data/code |
| Mozilla CA certificate bundle | current at build time | Mozilla/curl CA Extract terms |

The authoritative notices and license texts are contained in each downloaded
source archive. Build scripts and version declarations are under `tools/`.
Host-side build tools such as GCC, binutils, and GNU Make are not part of the
B1NIX source license merely because the build system downloads or invokes them.
If a build target installs or redistributes those tools, their own licenses
apply to that distribution.

## Binary Distribution

Placing downloaded source under `build/` does not by itself satisfy the
conditions for distributing binaries made from that source. A distributor of
a B1NIX image must determine which components are present in that image and
comply with every applicable license. This commonly includes:

- preserving copyright and license notices;
- providing the applicable license texts with the binary distribution;
- providing complete corresponding source code, build scripts, and any
  required installation information for GPL-covered binaries;
- satisfying LGPL relinking or source-code requirements for statically linked
  LGPL-covered libraries; and
- documenting local modifications to third-party code where required.

This file is an inventory, not a substitute for the complete license texts and
not legal advice.
