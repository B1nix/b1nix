# The archive key

The overlay's `Release` file is signed with an OpenPGP key. apt trusts the
overlay only through that signature: `b1nix-base-files` points the source at
`/etc/apt/keyrings/b1nix-archive.asc` with `Signed-By:`, and a repository
without a valid signature is refused. [plan.md](plan.md) has the reasoning;
this page is the procedure.

## The key

- **Primary key**: ed25519, certify only, kept offline. It signs nothing but
  its own subkeys.
- **Signing subkey**: ed25519, sign only, one year expiry, on the machine that
  publishes. This is what `SIGN_KEY` names. CI never holds the primary
  ([ci.md](ci.md)).

```sh
export GNUPGHOME=/path/to/offline/gnupg
gpg --quick-gen-key 'b1nix archive <archive@b1nix.invalid>' ed25519 cert never
gpg --quick-add-key <primary-fpr> ed25519 sign 1y
gpg --export-secret-subkeys <subkey-fpr>! >signing-subkey.gpg  # to the build host
```

## Publishing

```sh
SIGN_KEY=<subkey-fpr> sh tools/deb/publish-repo.sh
```

writes `Release.gpg` and `InRelease`, and exports the public key to
`b1nix-archive.asc` at the top of the repository. The fingerprint of the
primary is printed on the website and in the install guide. The installer
places the key file; a person adding the repository by hand downloads it and
compares the fingerprint first. `PKG-SMOKE` signs with a throwaway key on every
run and checks both that apt accepts the signature and that it refuses one
made by another key.

## Rotation

- **Routine (the subkey).** Add the new subkey under the same primary while the
  old one is still valid, re-export the public key and publish it, then sign
  with the new subkey. A machine verifies against the key file it holds, so the
  new subkey must reach machines before it signs anything. New installs get it
  in the `b1nix-archive.asc` the installer places; installed machines need a
  package that carries the key file, which `b1nix-base-files` does not do yet
  (its `README.keyring` says why) and must before the first rotation.
- **Compromise or a new primary.** Generate the new key, publish its
  fingerprint, and sign with both for one release cycle:
  `SIGN_KEY="<old> <new>"` puts two signatures on `Release`, and apt accepts a
  `Release` that any key in the file verifies. When every supported release
  ships the new key, the old one is dropped from `SIGN_KEY`, and on compromise
  it is revoked at once rather than at the end of the cycle.
- The rotation is recorded in the release notes with both fingerprints and the
  date the old key stops being used.
