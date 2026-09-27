# Verification samples

Deterministic inputs and the manifests that judge them, run by
`exo-verify samples run` (and by the `samples` step of `cargo exo-dev verify`).

- `manifests/*.toml` declare one sample each: its id, class, the contract it
  protects, its input and its oracles.
- `media/` holds committed inputs. Generated inputs are described by ffmpeg
  arguments in the manifest instead of being committed.
- `references/<id>.json` holds the reviewed expectation of a `metadata`
  oracle. Only `exo-verify samples run <id> --update-reference` rewrites it,
  and it prints what changed; CI never passes that flag.

`cargo exo-dev gen-av-sync-fixture` regenerates `media/clapper-golden.mp4`.
