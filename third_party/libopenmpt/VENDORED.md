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
the player plugins and bindings.

## Track16 changes

Marked "Track16" in the source; reapply them after an update.

- `libopenmpt/libopenmpt_ext.hpp`, `libopenmpt_ext_impl.hpp/.cpp`: the extension interface `track16_position`
  (`get_next_position`, `set_next_position`, `jump_to_position`, `rewind`): position changes without
  `set_position_order_row`'s seek, which simulates the song from its start (and allocates) on every call.
- `soundlib/Sndfile.h/.cpp`: `CSoundFile::Track16SetNextPosition`, which the interface uses (PlayState's fields are
  only open to CSoundFile).
- `soundlib/Sndmix.cpp` (`ProcessRow`): with endless repetition (repeat count −1) the visited-rows check is skipped.
  It only reset the visitor there, but that reset allocates, and the stutter loop revisits rows many times a second.
- `soundlib/ModChannel.h` (`track16Silent`), `soundlib/Fastmix.cpp` (`CreateStereoMix`) and
  `track16_position::set_channel_silent`: a voice the mixer leaves out while it plays on in every other respect
  (position, envelopes, effects, NNA voices, voice stealing). Mutes cannot do this: a muted channel is skipped
  entirely (its sample position stands still, it spawns no NNA voices and, in S3M, runs no effects), so instances
  that carry different channels drift apart. The player's stems use it; user mutes stay real mutes.
- `soundlib/Sndfile.h/.cpp` (`ResetPlayPos(bool)`) and `track16_position::rewind`: the rewind skips rebuilding the
  visited-rows table (it allocates and walks the whole song; about 2.5 ms per instance on the unit, so STOP with six
  instances dropped audio). With endless repetition the table is not consulted.
- `soundlib/Tables.cpp` (`CResampler::InitializeTablesFromScratch`): the windowed-FIR table is rebuilt only when its
  cutoff or window type changes, not on every interpolation-mode change (the INTERP knob stalled the audio thread
  for about 130 ms on the unit).

Built by [`cmake/libopenmpt.cmake`](../../cmake/libopenmpt.cmake), which follows the source lists of libopenmpt's
Makefile.

## Updating

1. Download the new `libopenmpt-<version>+release.makefile.tar.gz` from lib.openmpt.org and note its SHA-256.
2. Replace the directories listed above with the new ones (same selection).
3. Compare the new Makefile's `COMMON_CXX_SOURCES`, `SOUNDLIB_CXX_SOURCES` and `LIBOPENMPT_CXX_SOURCES` and the
   `MPT_WITH_*` options with `cmake/libopenmpt.cmake`.
4. Update this file and rebuild.
