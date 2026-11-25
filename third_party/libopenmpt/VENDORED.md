# libopenmpt (vendored)

- Version: **0.8.10** (svn r25841, tag `libopenmpt-0.8.10`, 2026-10-31)
- Source: <https://lib.openmpt.org/files/libopenmpt/src/libopenmpt-0.8.10+release.makefile.tar.gz>
- SHA-256 of that archive: `95073a10d33d0ed4352f41b68edccb9636002d2626f9fd2e00b191fcaabdd876`
  (computed on download; lib.openmpt.org does not publish checksums next to the files)
- Licence: BSD-3-Clause (`LICENSE`). Bundled decoders: miniz (MIT), minimp3 (CC0), stb_vorbis (public domain /
  MIT); their licence notes are in `include/*/` (stb_vorbis: at the end of `stb_vorbis.c`).

Only what the library build needs is kept: `common/`, `soundlib/`, `sounddsp/`, `src/`, the top-level files of
`libopenmpt/`, `include/{miniz,minimp3,stb_vorbis}/`, `build/svn_version/` and `build/dist.mk` (version
information), plus `LICENSE` and `README.md`. Not kept: `test/`, `examples/`, `openmpt123/`, `contrib/`, `doc/`,
the player plugins and bindings. The files are unmodified.

Built by [`cmake/libopenmpt.cmake`](../../cmake/libopenmpt.cmake), which follows the source lists of libopenmpt's
Makefile.

## Updating

1. Download the new `libopenmpt-<version>+release.makefile.tar.gz` from lib.openmpt.org and note its SHA-256.
2. Replace the directories listed above with the new ones (same selection).
3. Compare the new Makefile's `COMMON_CXX_SOURCES`, `SOUNDLIB_CXX_SOURCES` and `LIBOPENMPT_CXX_SOURCES` and the
   `MPT_WITH_*` options with `cmake/libopenmpt.cmake`.
4. Update this file and rebuild.
