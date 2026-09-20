# Monocypher — vendored copy

**Tag:** 4.0.2  
**Upstream:** https://github.com/LoupVaillant/Monocypher  
**Commit:** 0d85f98c9d9b0227e42cf795cb527dff372b40a4

## Files

| File | Source URL | SHA-256 |
|------|-----------|---------|
| `monocypher.c` | https://raw.githubusercontent.com/LoupVaillant/Monocypher/4.0.2/src/monocypher.c | `02174117935699d418443c75a558a287deb06ef8cf7c1adced61d9047d2f323d` |
| `monocypher.h` | https://raw.githubusercontent.com/LoupVaillant/Monocypher/4.0.2/src/monocypher.h | `fcaf6ed771358bb4f40fba016f6518ae86ec02b1b877d2cc35ad92d3a26fd7b3` |
| `monocypher-ed25519.c` | https://raw.githubusercontent.com/LoupVaillant/Monocypher/4.0.2/src/optional/monocypher-ed25519.c | `97d581639dfa72be08a6d57deb7d79b736be001cb416819cab196d22559d242b` |
| `monocypher-ed25519.h` | https://raw.githubusercontent.com/LoupVaillant/Monocypher/4.0.2/src/optional/monocypher-ed25519.h | `3a3035181f991a158d0e1c7567258f0bae8ba0f1f23c5512b4a1db1b3c9730ce` |

## Which function is used

`crypto_ed25519_check` from `monocypher-ed25519.c` / `monocypher-ed25519.h`.
This is the **SHA-512** variant and is **RFC 8032 compatible** (same as pynacl / libsodium).
Do NOT use `crypto_eddsa_check` from `monocypher.c` — that uses BLAKE2b and is NOT RFC 8032.

## License

Monocypher is **dual-licensed: `BSD-2-Clause OR CC0-1.0`** — a recipient chooses
either licence. ExoSnap does not relicense it exclusively under one of the two;
both remain available downstream.

The upstream licence text is vendored unmodified as `LICENSE`, from
https://raw.githubusercontent.com/LoupVaillant/Monocypher/4.0.2/LICENCE.md
(SHA-256 `a5781770269d2516e52ba4863f790c10a16da4089a1e81823aee19ff1e9026b0`,
line endings normalized to LF by the repository `.gitattributes`). The same
notice is repeated in the header of each vendored source file, naming the
per-file contributors and years.
