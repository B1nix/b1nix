# Prebuilt port artifacts

Three ports cost more than everything else in the tree put together, and none of
them change from one checkout to the next unless their recipe does:

| port  | installed | packed | what it is |
|-------|-----------|--------|------------|
| skia  | 492 MB    | 63 MB  | 2D graphics, built with the Ganesh backend |
| mesa  | 216 MB    | 40 MB  | software OpenGL (OSMesa, softpipe, virgl) |
| musl  | 28 MB     | 2.7 MB | the C library every b1nix binary links against |

`tools/prebuilt.sh` fetches those instead of compiling them, and falls back to
compiling when it cannot. Nothing about the build changes if the cache is empty
or unreachable — that is the point.

## How a stale artifact is made impossible

Each artifact is named for a **key**: a hash over the port's build script, the
compiler wrappers it calls, the host compiler's version banner, the target
architecture and the C++ standard library selection. Change any of those and the
key changes, so the old tarball is simply not what is being asked for and the
port is built from source.

The key answers "is this the same recipe". `tools/packages/prebuilt.lock` answers
"is this the same bytes": it records `artifact sha256`, and a download whose hash
does not match is deleted rather than used. Between them, a cache hit is only
possible when the result would have been identical anyway.

## Using it

```sh
tools/prebuilt.sh list              # ports it knows about
tools/prebuilt.sh key skia          # the key for the current recipe
tools/prebuilt.sh fetch skia        # unpack a matching artifact, or exit 1
tools/prebuilt.sh pack skia         # package what is built, for uploading
```

The Makefile already calls `fetch` before building musl, Mesa and Skia. To
ignore the cache entirely — when bisecting a build problem, for instance:

```sh
B1NIX_PREBUILT_OFF=1 make iso
```

To use a directory of artifacts instead of the network, which is what a second
machine on the same desk wants:

```sh
B1NIX_PREBUILT_DIR=/mnt/share/b1nix-prebuilt B1NIX_PREBUILT_URL= make iso
```

## Publishing

Artifacts land in `build/<arch>/prebuilt-out/` and are uploaded as release assets
of a separate repository — `B1NIX_PREBUILT_URL` points at it, and defaults to
`github.com/B1nix/b1nix-prebuilt`. Releases rather than the repository itself
because a 63 MB file has no business in git history, and release assets have
neither the 100 MB file limit nor LFS's bandwidth quota.

The workflow after changing a recipe is:

1. build normally, so the port is compiled from the new recipe;
2. `tools/prebuilt.sh pack <port>` — writes the tarball and records its hash;
3. upload the tarball to the prebuilt repository's release;
4. commit `tools/packages/prebuilt.lock`.

Step 4 is what makes the artifact usable: without the recorded hash, `fetch`
refuses to trust anything it downloads and every machine builds from source —
which is inconvenient, and correct.

## What is deliberately not cached

The LLVM runtimes build tree (2.6 GB of intermediates) and every Alpine package.
The first is regenerable and enormous; the second already has a cache of its own
in `build/<arch>/pkgcache`, pinned by `tools/packages/alpine.lock`.
