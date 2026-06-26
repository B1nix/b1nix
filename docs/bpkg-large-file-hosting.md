# bpkg — Hosting Large Binary Files

**Status:** research / decision record. **Last verified:** 2026-06-24 (limits below were checked
against current vendor docs that month — they change, re-verify before relying on them).

## Problem

`bpkg`'s client is a POSIX-sh script that runs `curl -L` + `busybox tar/gzip/sha256sum`. It reads a
plain-text INDEX (`name version arch sha256 URL [deps]`) and, for each package, downloads **one
tarball** from the `URL` field over HTTPS, checks the sha256, and extracts it. So any hosting option
**must** provide:

- a **stable, public, direct-download HTTPS URL** per file (no JS, no login, no API token, plain `curl -L`);
- ideally **no egress cost** and **no surprise bandwidth cap**;
- durability (no link rot), and a trivial one-time publish step.

Small packages already ship via **jsDelivr** fronting the `B1nix/b1nix-pkgs` GitHub repo
(`https://cdn.jsdelivr.net/gh/B1nix/b1nix-pkgs@main/pkgs/<arch>/<tarball>`). That's great for small
files but **jsDelivr caps individual files at ~20 MB** (docs say 50 MB, the live limit is 20 MB — see
table), so the big toolchains don't fit.

**Big files to serve:** stripped llvm ~52 MB, clang ~27 MB, rust ~97 MB; unstripped shared libs
libLLVM.so ~184 MB, libclang-cpp ~83 MB; future kernel + full disk-install image. Range ~30 MB to a
few hundred MB, total maybe 1–3 GB, **low-to-moderate** download volume.

---

## Comparison

| Option | Max file size | Total / bandwidth limits | Plain stable direct `curl` URL? | Cost (free tier) | Durability / risk | Fit for bpkg |
|---|---|---|---|---|---|---|
| **jsDelivr (gh repo)** *(current, small files)* | **~20 MB/file** (doc says 50 MB; live cap 20 MB) | Generous CDN, fair-use only | ✅ `cdn.jsdelivr.net/gh/<o>/<r>@<ref>/<path>` | Free | High (large public CDN) | ✅ small only — **too small for toolchains** |
| **GitHub Releases** *(baseline to beat)* | **2 GiB/file**, ≤1000 assets/release, no total cap | No documented bandwidth cap; anon downloads share GitHub's unauth rate limits (tightened May 2025, ~60 req/h/IP-ish for API/raw — release-asset downloads themselves are not metered the same way but are best-effort) | ✅ `github.com/<o>/<r>/releases/download/<tag>/<file>` (302→CDN, `curl -L` follows) | Free | High | ✅ works, **already the fallback** — user wants to avoid it |
| **GitHub raw + Git LFS** | raw blob cap 100 MB; LFS file up to 2 GB | **LFS free bandwidth = 1 GB/month**, then paid; **jsDelivr/raw return the LFS _pointer_ text, not the file** | ⚠️ raw/jsDelivr serve a `version https://git-lfs…` pointer, not the binary | Free storage 1 GB / bw 1 GB-mo | Low for this use | ❌ pointer problem + tiny bw quota — **unusable** |
| **Cloudflare R2** | 5 TiB/object (S3 multipart); single PUT 5 GiB | **Egress free, all methods incl. r2.dev & custom domain**; free tier 10 GB storage + 1M Class-A + 10M Class-B ops/mo, no expiry | ✅ `https://pub-<hash>.r2.dev/<key>` (or custom domain) — plain GET, no token | **Free** within tier; storage $0.015/GB-mo after; **$0 egress** | High (Cloudflare) | ✅✅ **best fit** |
| **Backblaze B2 (+ Cloudflare)** | 10 TB/file | Free egress only up to **3× monthly stored GB**, then $0.01/GB; **free via Cloudflare Bandwidth Alliance** if fronted by a CF custom domain | ✅ `https://f<NNN>.backblazeb2.com/file/<bucket>/<key>` (machine-ID host can move); CF custom domain is stable | No perpetual free storage tier; ~$6/TB-mo | Medium-high | ⚠️ ok, but R2 is simpler & cheaper at this scale |
| **Hugging Face Hub** | 200 GB recommended / **500 GB hard** per file | Free public storage "best-effort"; fair-use ("be useful to the community"); CloudFront-served | ✅ `https://huggingface.co/<repo>/resolve/main/<file>` — plain `curl -L`, **no token** for public repos | Free (public, best-effort) | Medium (ToS is ML-oriented; non-ML binaries = grey area, abuse mitigations exist) | ⚠️ technically works, **ToS-mismatch risk** — backup only |
| **archive.org (Internet Archive)** | very large (multi-GB) | Throttles >3–4 parallel conns | ⚠️ `archive.org/download/<item>/<file>` is the stable form; numeric `ia*.us.archive.org` hosts move | Free | Medium (items can be darked; slower) | ⚠️ durable-ish, slow, not ideal |
| **GitLab generic package registry** | default 100 MB (admin-set; gitlab.com = 5 GB) | — | ⚠️ public download exists but API/token-centric; awkward unauth URL | Free | Medium | ❌ token-friction, not clean |
| **npm + unpkg/jsDelivr-npm** | bound by same CDN per-file caps (~20–50 MB) | — | ✅ but same size cap as jsDelivr | Free | High | ❌ same small-file cap |
| **Cloudflare Pages** | **25 MiB/file** | — | ✅ | Free | High | ❌ 25 MB cap → too small |
| **Bunny CDN / Bunny Storage** | large | Pay-per-GB egress (~$0.01/GB), $1/mo min | ✅ pull-zone URL | ~$1/mo min, not free | High | ⚠️ paid; unnecessary at this scale |
| **Self-hosted VPS / origin + CF** | unlimited | your bandwidth/$$ | ✅ | $5+/mo + ops | depends on you | ❌ **not warranted** (see below) |

---

## Recommendation

### Primary: **Cloudflare R2 public bucket** (no Releases, no crutch)

R2 is the cleanest fit and beats GitHub Releases on every axis the user cares about:

- **Zero egress cost, all access methods** — including the free `pub-<hash>.r2.dev` subdomain and any
  custom domain. There is no bandwidth meter to blow through (unlike LFS's 1 GB/mo or B2's 3× cap).
- **Free tier never expires**: 10 GB storage + 1M Class-A + 10M Class-B ops/month. The whole bpkg
  toolchain set (~1–3 GB) sits inside 10 GB; downloads are Class-B reads, nowhere near 10M/mo at hobby
  volume. Past the tier it's $0.015/GB-mo storage and **still $0 egress**.
- **Plain direct URL, no token**: a public bucket serves `https://pub-<hash>.r2.dev/<key>` to a bare
  `curl -L` — no JS, no login, no API. The bpkg client needs **zero changes**.
- **Large files are a non-issue**: 5 TiB/object; a 5 GiB single PUT covers any toolchain or disk image.
- **Durable**: Cloudflare-operated; bucket + key are stable as long as you keep the account.

**One caveat to handle up front:** the free `r2.dev` URL is officially a *development* URL (no edge
cache; Cloudflare reserves the right to rate-limit abuse and doesn't SLA it). For a hobby project this
is fine, but the clean, durable move is to **bind a custom domain** (a subdomain you already control,
e.g. `pkgs.b1nix.<tld>`, added to Cloudflare) to the bucket. That gives a stable, cacheable,
production-grade URL — still $0 egress — and decouples the public URL from the random bucket hash.
**Do not** CNAME to `r2.dev` (unsupported); attach the custom domain to the bucket directly.

### Backup: **GitHub Releases** (keep it as-is)

It already works (`stage-toolchains.sh` uses it) and gives 2 GiB/file behind stable
`github.com/<o>/<r>/releases/download/<tag>/<file>` URLs that `curl -L` follows. Keep it as the
fallback / mirror so a single provider outage or account issue can't brick `bpkg install`. The only
downside vs R2 is that asset downloads are best-effort and share GitHub's (tightening) unauthenticated
rate limits — fine at hobby volume.

Second-string backups, in order: **Hugging Face Hub** `resolve/main` URLs (works token-free for public
repos, but the ToS is ML-oriented so general toolchains are a grey area — use only if pressed) and
**archive.org** (durable, slow, throttles parallelism).

### Is a self-hosted CDN needed? **No.**

A VPS or self-run origin adds monthly cost, an uptime/patching burden, and a single point of failure —
to serve 1–3 GB at low volume that R2 already serves for free with zero egress. Self-hosting is not
warranted at this scale and would be the opposite of "no hacks/crutches." Skip it.

### Reject (and why)

- **Git LFS** — `raw`/jsDelivr hand back the **pointer text**, not the binary, and the **free bandwidth
  is only 1 GB/month**. Dead on arrival for a public download path.
- **Chunking under the jsDelivr cap** — splitting each tarball into <20 MB pieces, listing them all in
  the index, and `cat`-ing them back client-side. It *is* a real technique (Git/GitHub themselves chunk
  LFS; package mirrors split ISOs), so it's not illegitimate per se — **but for bpkg it's a crutch**:
  it bloats the index (N rows + a manifest per package), adds reassembly + per-chunk-vs-whole-file hash
  logic to a script whose whole virtue is being tiny, and multiplies failure modes (one missing chunk =
  silent corruption). With R2 available there is **no reason** to take on that complexity. Don't do it.
- **Cloudflare Pages (25 MiB), npm/unpkg (~CDN cap), GitLab (token friction), Bunny (paid)** — each
  fails one hard requirement (size, token-free, or free).

---

## How it plugs into bpkg

The INDEX format is unchanged — only the `URL` column points at R2 for big packages:

```
# name        version  arch       sha256                URL
llvm           18.1.0   x86_64-b1nix  <sha256>  https://pkgs.b1nix.example/x86_64-b1nix/llvm-18.1.0.tar.gz
libLLVM.so     18.1.0   x86_64-b1nix  <sha256>  https://pkgs.b1nix.example/x86_64-b1nix/libLLVM-18.1.0.tar.gz
# small packages stay on jsDelivr:
foo            1.2.3    x86_64-b1nix  <sha256>  https://cdn.jsdelivr.net/gh/B1nix/b1nix-pkgs@main/pkgs/x86_64-b1nix/foo-1.2.3.tar.gz
```

The client already does `curl -L "$URL"` + `sha256sum` — **no client change needed**. Mixed hosting in
one index is fine; the URL column is per-row.

**One-time publish step** (mirrors today's `bpkg-publish.sh` flow, big files only):

1. Create a public R2 bucket; optionally attach a custom domain (`pkgs.b1nix.<tld>`) to it for a stable
   URL (recommended over the raw `pub-<hash>.r2.dev`).
2. Upload the tarball: `aws s3 cp llvm-…tar.gz s3://<bucket>/x86_64-b1nix/ --endpoint-url <r2-s3-endpoint>`
   (or `rclone`, or the dashboard) — auth needed only to **upload**, never to download.
3. Compute `sha256sum`, append the row to `pkgs/index` with the R2 URL, push the index (the index
   itself stays small → still served via jsDelivr).

Optionally extend `tools/packages/stage-toolchains.sh` (which already special-cases the big toolchains
to GitHub Releases) to point at R2 instead, keeping Releases as a mirror.

### Caveats

- **r2.dev is a dev URL** (no SLA, no edge cache, abuse-rate-limited). Use a **custom domain** for the
  durable public path. Do not CNAME to r2.dev.
- **Keep a mirror.** Publish big files to **both** R2 and GitHub Releases so one provider problem
  doesn't break installs (cheap insurance; the index can list a primary + the client/script can retry a
  mirror URL).
- **ToS:** R2 and GitHub Releases both permit hosting your own project's release binaries. **Hugging
  Face** is intended for ML artifacts — hosting general toolchains there is a grey area; treat as
  last-resort only.
- **Link rot:** R2 keys and a custom domain are stable while the account/domain live; GitHub Releases
  URLs are stable per tag. Both are low-risk; the mirror covers the rest.
- **Rate limits:** none meaningful for R2 at hobby volume; GitHub unauthenticated limits are tightening
  but don't bite normal release-asset downloads. Re-verify the jsDelivr per-file cap and GitHub
  rate-limit numbers before any big change — these move.

---

## Sources (verified 2026-06)

- jsDelivr per-file size limit (20 MB live vs 50 MB doc): https://github.com/jsdelivr/jsdelivr/issues/18268 ; https://www.jsdelivr.com/documentation
- GitHub Releases (2 GiB/file, 1000 assets, no total cap): https://docs.github.com/en/repositories/releasing-projects-on-github/about-releases
- GitHub unauthenticated rate-limit change (May 2025): https://github.blog/changelog/2025-05-08-updated-rate-limits-for-unauthenticated-requests/
- Cloudflare R2 pricing / free tier / zero egress: https://developers.cloudflare.com/r2/pricing/
- Cloudflare R2 public buckets / r2.dev dev-URL caveat: https://developers.cloudflare.com/r2/buckets/public-buckets/
- Git LFS billing (1 GB storage + 1 GB/mo bandwidth free): https://docs.github.com/billing/managing-billing-for-git-large-file-storage/about-billing-for-git-large-file-storage
- jsDelivr/raw serve LFS pointer not file: https://github.com/jsdelivr/jsdelivr/issues/18235
- Backblaze B2 + Cloudflare Bandwidth Alliance free egress: https://www.backblaze.com/blog/backblaze-and-cloudflare-partner-to-provide-free-data-transfer/
- Hugging Face storage limits (best-effort public, 500 GB/file hard cap, fair-use): https://huggingface.co/docs/hub/en/storage-limits
- Internet Archive download URL form / throttling: https://help.archive.org/help/how-to-download-files/
- GitLab generic packages (100 MB default cap, token-centric): https://docs.gitlab.com/user/packages/generic_packages/
- Cloudflare Pages 25 MiB per-file limit: https://developers.cloudflare.com/pages/platform/limits/
