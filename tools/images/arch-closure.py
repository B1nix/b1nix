#!/usr/bin/env python3
"""Resolve an Arch package dependency closure from the repository databases.

pacman's job, done with the same inputs pacman uses and none of its privileges:
the repo `*.db` tarballs, and the list of names the tree already has.  Prints
one line per package that has to be downloaded:

    <name> <repo> <filename> <sha256> <version>

A dependency that cannot be satisfied is a hard error, because the alternative
is an image that is missing a library and fails at run time for a reason that
looks nothing like its cause.
"""
import argparse
import io
import os
import sys
import tarfile

# A dependency string is "name", "name>=1.2", "name=1.2-3" or a soname such as
# "libseat.so=1-64".  Only the name part decides what package satisfies it --
# versions are not compared here, because every package comes from the same
# snapshot of the same repositories and there is nothing to choose between.
_DELIMS = ("<", ">", "=")


def dep_name(spec):
    spec = spec.strip()
    if not spec:
        return ""
    # A description follows a ':' in an optdepend; deps never carry one, but
    # cutting it costs nothing and makes the parser total.
    spec = spec.split(":")[0].strip()
    cut = len(spec)
    for d in _DELIMS:
        i = spec.find(d)
        if i != -1:
            cut = min(cut, i)
    return spec[:cut].strip()


def parse_desc(text):
    """A pacman `desc` file: %KEY% on its own line, values until a blank line."""
    fields = {}
    key = None
    for line in text.splitlines():
        if line.startswith("%") and line.endswith("%") and len(line) > 2:
            key = line
            fields.setdefault(key, [])
            continue
        if not line.strip():
            key = None
            continue
        if key:
            fields[key].append(line.strip())
    return fields


def load_db(path, repo, index, provides):
    with tarfile.open(path, "r:*") as tf:
        for member in tf:
            if not member.isfile() or not member.name.endswith("/desc"):
                continue
            fh = tf.extractfile(member)
            if fh is None:
                continue
            f = parse_desc(io.TextIOWrapper(fh, encoding="utf-8",
                                            errors="replace").read())
            names = f.get("%NAME%")
            if not names:
                continue
            name = names[0]
            if name in index:
                continue                     # first repo on the list wins
            index[name] = {
                "repo": repo,
                "file": (f.get("%FILENAME%") or [""])[0],
                "sha256": (f.get("%SHA256SUM%") or [""])[0],
                "version": (f.get("%VERSION%") or ["?"])[0],
                "depends": [dep_name(d) for d in f.get("%DEPENDS%", [])],
            }
            for prov in f.get("%PROVIDES%", []):
                p = dep_name(prov)
                if p and p not in provides:
                    provides[p] = name


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--installed", required=True,
                    help="file of names the tree already provides, one per line")
    ap.add_argument("dbs", nargs="+", help="repo .db files, in priority order")
    args = ap.parse_args()

    index, provides = {}, {}
    for path in args.dbs:
        repo = os.path.basename(path)
        if repo.endswith(".db"):
            repo = repo[:-3]
        load_db(path, repo, index, provides)

    installed = set()
    with open(args.installed, encoding="utf-8") as fh:
        for line in fh:
            line = line.strip()
            if line:
                installed.add(line)

    seeds = os.environ.get("PACKAGES", "").split()
    selected, order = set(), []
    queue = list(seeds)
    seen = set(seeds)
    while queue:
        want = queue.pop(0)
        name = want
        if name not in index:
            real = provides.get(name)
            if real is None:
                # Nothing in the repos provides it.  If the tree already has
                # it, that is the answer; otherwise the closure is broken and
                # saying so here beats discovering it at boot.
                if name in installed:
                    continue
                sys.exit("arch-closure: unresolvable dependency: %s" % name)
            name = real
        if name in selected:
            continue
        selected.add(name)
        order.append(name)
        for dep in index[name]["depends"]:
            if not dep or dep in installed or dep in selected:
                continue
            if dep in seen:
                continue
            seen.add(dep)
            queue.append(dep)

    for name in order:
        # A seed is downloaded even if the tree claims to have it, so that
        # asking for a package always gets that package; anything else is only
        # fetched when it is genuinely absent.
        if name in installed and name not in seeds:
            continue
        p = index[name]
        if not p["file"]:
            sys.exit("arch-closure: %s has no %%FILENAME%% in the index" % name)
        print(name, p["repo"], p["file"], p["sha256"], p["version"])


if __name__ == "__main__":
    main()
