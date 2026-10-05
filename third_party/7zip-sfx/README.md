# Pinned official installer SFX

- Package: **7-Zip Extra 9.20**, Igor Pavlov (2010); this is a deliberately pinned historical module, not the latest 7-Zip.
- Official binary archive: https://www.7-zip.org/a/7z920_extra.7z
- Official corresponding source: https://www.7-zip.org/a/7z920.tar.bz2
- Member: `7zS.sfx`, 140,288 bytes.
- SHA-256: `998F55C1B61BE2C7E0C5F11673B03C36BD7BB941273FCF956CAA1B746C08178F`.
- License: GNU LGPL 2.1 or later; see the unchanged upstream License.txt and COPYING.LGPL-2.1.txt.
- Upstream package readme: upstream-readme.txt; machine-readable pin: module.json.

The packager refuses other module bytes. A module upgrade must update the pin, review upstream changes and rerun the complete lifecycle/corruption probes and all four candidates. A hash pin makes module selection repeatable and verifiable; it does not by itself make the whole compiler output bit-for-bit reproducible, and it is not a signature or a claim that an old module is free of security defects.

The module is an installer SFX (not the regular extraction-only 7z.sfx). The project supplies a UTF-8 configuration with an explicitly quoted extraction-directory executable path. Archive data uses LZMA without BCJ filters for module compatibility. Only the generated, controlled release archive is embedded; recipients should obtain the single EXE from a trusted project release and verify its checksum.

Distribute the combined EXE together with project source/build scripts or make these available to allow rebuilding/repackaging with replacement dynamically linked Qt libraries. SFX licenses, Qt licenses and dependency notices are also inside the payload. Cleanup applies to the SFX-created extraction directory on normal main-process exit; crashes, forced termination, antivirus locks or still-running child tools can leave residue. User AppData/tool caches are not removed.
