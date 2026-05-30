# r300 legacy-feature archaeology

Holding branch for r300 driver features dropped from upstream Mesa, kept for
later analysis.  This branch is intentionally NOT merged to main: it captures
removed-feature source and mining notes so the RS482 graphics-as-compute work
can evaluate them without carrying speculative or unbuilt code on main.

## Source of truth

The per-removal verdicts live in the steinmarder finding
`src/re/r300/findings/active/2026-05-29-rs482-removed-mesa-r300-feature-mining-and-lowering-elucidation.md`,
which extracts each feature at its own era by `git log -S` archaeology.  This
manifest is the mesa-side extraction plan derived from that finding.

## Verdict summary (full reasoning in the finding)

| Removed feature | Era | Verdict |
|---|---|---|
| KILL transform simplification | 21.3.4 | Superseded -- VDPAU dead code; no value |
| radeon_rename_regs / radeon_pair_regalloc | 25.0 | Superseded by NIR register allocation |
| finalize_nir | 25.1 | Modernization cleanup; nothing to re-add |
| ATI2N texture format | 25.0 | Opt-in already landed (R300_EXPERIMENTAL_ATI2N); the open hardware question is a sampler probe, not a source extraction |
| MPEG/video shader decode (r300_video_context / g3dvl) | g3dvl era | HIGHEST mining value -- the decode kernels are a graphics-as-compute workload |

## High-value extraction target: g3dvl shader video decode

RS482 wired the shared g3dvl video layer through `r300_get_video_param`
(mesa-21.3.3 `src/gallium/drivers/r300/r300_context.c:437`, calling
`vl_profile_supported` / `vl_video_buffer_max_size` / `vl_level_supported` via
`vl/vl_decoder.h` and `vl/vl_video_buffer.h`).  The decode kernels themselves
(IDCT, motion compensation, the shader-based MPEG path) live in the shared g3dvl
under `src/gallium/auxiliary/vl/` at the g3dvl era, not in the r300 driver
directory.

Extraction commands (run from the mesa fork; the `upstream` remote carries 149
tags across mesa-19 .. mesa-21):

    git log -S vl_mpeg12_decoder --oneline -- src/gallium/auxiliary/vl/
    git show <tag>:src/gallium/auxiliary/vl/vl_mpeg12_decoder.c
    git show mesa-21.3.3:src/gallium/drivers/r300/r300_context.c   # r300_get_video_param wiring

Mining sources: `/tmp/mesa-mining/mesa-21.3.3` (pre-NIR r300, g3dvl wiring live)
and the `upstream` remote tags `mesa-19.*` .. `mesa-21.*`.

Deep extraction is DEFERRED: it spans older tags and the shared g3dvl, and the
goal is graphics-as-compute pattern mining onto the RS482 64-ALU/32-TEX substrate
(the finding's next_experiment), not a clean file copy onto a modern driver.

## Preserved build-hygiene patch

`r600-va-build-warning-suppression.patch` is an unfinished GCC warning-suppression
patch (`r600_buffer_common.c` `-Warray-bounds`, `va/context.c`
`-Wdeprecated-declarations`) recovered from a stray r2vb scaffold worktree.  The
real refactor hunk did not apply (it left a `.rej`), and the intent is on neither
main nor the clang-22 r300 build path that has been validated.  It is kept here
to evaluate under a warnings-as-errors r600/va build before deciding whether main
needs it; it is not applied.
