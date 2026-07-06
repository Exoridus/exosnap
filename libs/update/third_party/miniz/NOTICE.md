# miniz — vendored copy

**Tag:** 3.1.2
**Upstream:** https://github.com/richgel999/miniz
**Release asset:** https://github.com/richgel999/miniz/releases/download/3.1.2/miniz-3.1.2.zip
(asset SHA-256 per GitHub release metadata: `f0446d863f9c19926ad9483c523fdc42e42b8d4a6a431d27e09d49c79a140d9a`)

## Files

Single-file amalgamation from the release archive. `miniz.c` is byte-identical
to upstream. `miniz.h` was re-formatted with the repo `.clang-format` (no code
changes) because the format gate covers `libs/**/*.h`; the upstream hash is
listed for provenance.

| File | SHA-256 (as vendored) | SHA-256 (upstream original) |
|------|----------------------|-----------------------------|
| `miniz.c` | `e2c1aeb66eef9191d8c3feb164db2def2335a61d039bf04ed849f6b042433b30` | same (unmodified) |
| `miniz.h` | `ccd6c72fb1aa149c23b2ebf13bb03ecd734ead0a81b30a806149c9db4e377afd` | `b53b62ed122e559b8f679e3cb787a0b0035fe87a58f909da0e44931678f4e85f` |

## Which API is used

`mz_zip_reader_*` (zip file open/enumerate/extract) via
`libs/update/src/zip_extract.cpp`. Also used directly by
`libs/update/tests/test_zip_extract.cpp` (`mz_zip_writer_*`) to build fixture
archives.

## License

miniz is MIT-licensed. Upstream license text vendored unmodified as
`LICENSE` alongside this notice.
