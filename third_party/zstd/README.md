# Zstandard decoder (pinned 1.5.7)

This directory contains the decoder-only amalgamation used by the existing
in-process Ouga Payload extractor for operation type 14 (ZSTD). It is compiled
statically into the Qt application and related tests; no new executable, DLL,
Python module, user-configured path or UI operation is added.

## Source and reproducibility

- Upstream: https://github.com/facebook/zstd
- Fixed source tag: `v1.5.7` (not a claim about the latest release).
- Archive: https://github.com/facebook/zstd/archive/refs/tags/v1.5.7.zip
- Archive SHA-256:
  `7897bc5d620580d9b7cd3539c44b59d78f3657d33663fe97a145e07b4ebd69a4`
- From `build/single_file_libs` in that archive:

  ```text
  python combine.py -r ../../lib -x legacy/zstd_legacy.h -o zstddeclib.c zstddeclib-in.c
  ```

Generated source is normalized to LF, with no other changes. `zstd.h`,
`zstd_errors.h` and the BSD `LICENSE` are copied unchanged from upstream
`lib/zstd.h`, `lib/zstd_errors.h` and `LICENSE`.
Repository attributes keep LF endings so hashes survive Windows checkout.

| File | SHA-256 |
| --- | --- |
| `zstddeclib.c` | `5eb38abe0a7eea13674b312a006df72002d4192f0f0b4d09e6f03fb927a1f3d0` |
| `zstd.h` | `9b4bc8245565c98ccfc61c07749928b57e7c0f6fddb0530c4f6aa1971893d88b` |
| `zstd_errors.h` | `66a8c3f71d12ea6e797e4f622f31f3f8f81c41b36f48cad4f5de7d8bfb6aac0a` |
| `LICENSE` | `7055266497633c9025b777c78eb7235af13922117480ed5c674677adc381c9d8` |

## License and validation boundary

The BSD alternative is selected. Preserve the upstream notices in the source
and accompany deployed binaries with `licenses/zstd-BSD.txt` via `deploy.ps1`.
The amalgamation includes xxHash code under Zstandard's BSD/GPL dual-license;
this does not select the GPL alternative.

The extractor bounds decoded operations and windows to 256 MiB, uses each
worker's reusable decode context and rejects dictionary-required, skippable,
concatenated, truncated and trailing-data frames. It verifies operation
SHA-256, exact decoded length and extent coverage before publishing an image.
The existing checks before flashing are unchanged.

Tests contain a fixed 292-byte genuinely compressed, checksummed frame generated
with this same upstream version (`ZSTD_compress2`, level 3, checksum enabled),
plus test-only RAW/RLE frame builders. A compressor is not linked into tests or
the application. The format reference is upstream
`doc/zstd_compression_format.md`; invalid-frame and GUI tests require no phone
or external payload tool.
