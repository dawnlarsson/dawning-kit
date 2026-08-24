# Security Policy

## Reporting a vulnerability

Report security issues privately through
[GitHub's private vulnerability reporting](https://github.com/dawnlarsson/dawning-kit/security/advisories/new),
or on [the Discord server](https://discord.gg/cxRvzUyzG8) if you would rather
start a conversation first.

Please do not open a public issue for anything that affects the integrity of a
produced binary or kernel image until it has been addressed.

## What this project guarantees

### Kernel source integrity

`linux/script/kernel_setup` pins a PGP signature for the exact kernel tarball it
downloads and verifies the archive against it. Verification is a gate: if the
key fetch, the decompression, or the signature check fails, the script exits and
nothing is extracted or built.

The signing keys are fetched at build time with `gpg --locate-keys` for
`torvalds@kernel.org` and `gregkh@kernel.org`. This means the build depends on
network trust at that moment. If you need a stronger guarantee, import and pin
the key fingerprints into your own keyring ahead of time and run the build
offline.

### What it does not guarantee

- **Profiles are executed as root.** `linux/build.sh` runs `eval` on the `pre`
  and `post` values read out of the profile files during a `sudo` build. Adding
  a profile is equivalent to running arbitrary code as root. Only use profiles
  you have read.
- **Generated executables are not sandboxed.** `bit.sh` writes whatever bytes
  the generator function produces. It marks the segment read+execute rather than
  read+write+execute, but it does not inspect or validate the code.
- **`doc.sh` escaping is best effort.** Text nodes, code blocks and attribute
  values are escaped, and URL schemes are restricted to an allowlist. Lines that
  already contain an HTML tag are deliberately passed through unescaped, so do
  not point `doc` at untrusted Markdown and then serve the result on a domain
  that matters without reviewing the output.

## Supported versions

This project tracks `main`. Fixes land there; there are no maintained release
branches.
