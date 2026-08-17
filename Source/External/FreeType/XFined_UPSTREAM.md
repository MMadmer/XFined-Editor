# FreeType upstream provenance

- Version: 2.14.3
- Release date: 2026-03-22
- Official release page: https://sourceforge.net/projects/freetype/files/freetype2/2.14.3/
- Authoritative mirror: https://download-mirror.savannah.gnu.org/releases/freetype/
- Imported archive: `freetype-2.14.3.tar.xz`
- Archive SHA-256: `36bc4f1cc413335368ee656c42afca65c5a3987e8768cc28cf11ba775e785a5f`
- Selected license: FreeType Project License (`docs/FTL.TXT`)

All 855 files extracted from the release archive are stored byte-for-byte
unchanged. This provenance file is the only XFined-specific file in the
directory. The CMake integration uses the exact Windows source set from the
upstream 2.14.3 CMake target and its bundled zlib implementation; optional
bzip2, PNG, HarfBuzz, and Brotli integrations remain disabled.
