# Resume state at the usage boundary

Written 2026-09-03 14:25 PDT, with the budget nearly exhausted and reset at
18:00 local. It records where every lane stood so a fresh session recovers
from artifacts rather than from a transcript. The scope itself is in
`docs/hardware/r3v-rb2d-fill-and-rs485m-identity-program-ledger.md`; this
file is only the boundary snapshot.

## What survives a session ending, and what does not

    pushed branch and PR      survives
    commit in a worktree      survives on this machine, invisible to others
    uncommitted worktree file survives on disk, owned by nobody
    subagent context          gone
    CronCreate schedule       gone if the session exits before it fires

Every running agent was told to commit whatever it had, prefix the subject
`WIP:` when incomplete, push, and report status. Treat a pushed branch as
the authority for where its lane stopped. Do not resume an agent's task
from the task text it was given; read what the branch actually contains,
because the two diverge once an agent has worked.

## Merged during this tranche

    mesa            9b3b84fb945  PR 2116, RB2D linear span planner
    steinmarder     192727ab8    PR 576, seal workflow PyYAML dependency
    steinmarder     1a3475a7b    PR 578, RS485M identity migration, 241 files
    vostro1000-re   e59ca7971b   PR 667, RS485M identity migration

## Open, with what each still needs

    mesa 2118   RS485M identity and the platform arming gate.
                Three repairs outstanding: the at_rest specimen values still
                duplicated in r300_family_facts, the negation guard that is
                clause-wide and must bind to the binding verb, and the
                rebase onto 9b3b84fb945 with profiles 3, 4 and 5. Its PR
                body still carries superseded formulations. Every review
                thread needs a reply and a resolution.

    mesa 2119   This ledger. Merge after 2118.

    mesa 2117   Superseded, not to be merged. Its branch is a defect proof
                and a source of cherry-picks for the two replacements.

    stein 575   POWERPLAY and SCLK rework. Last in the steinmarder order,
                because it touches findings and generated migration state
                that the identity migration just moved.

    stein 577   0.8.13 deployment seal. Needs recapture against the RS485M
                platform tuple, and needs the target box.

    vostro 668  UMA/GART specimen strings, with the test authority restored
                so the code states RS485M and the tests verify it.

    vostro 669  Option ROM static decomposition model, 27 facets in four
                layers, bounded to COMBIOS by the image's own discriminator.

## Branches an agent owned when the budget ran out

Each was told to push. Verify with `git fetch --prune` and read the branch;
its state is the truth.

    build/review_thread_frontier_owner_reconciliation
        Repairs the pre-existing review-thread frontier failure. Evidence
        owner build-infra/packaging/mesa-gororoba-debug-optimized/PKGBUILD
        advanced past the recorded 217378421cb. Regenerate through the
        generator, never by editing a digest. This blocks any profile-4
        evidence build under REPRODUCIBLE_RUN=1, so it gates the
        qualification ladder.

    r300/legacy_2d_register_authority
        Moves twelve legacy 2D registers and their fields out of
        r300_rb2d_fill.c into radeon_legacy_2d_reg.h. The proof is byte
        identity: the retained direct-write golden and the span-plan IB must
        not move. Needs a rebase onto 9b3b84fb945 and a re-proof after 2118.

    r300/rs485m_firmware_chip_identity
        PR 2118 above. An agent was driving the naming scan to zero from 128
        findings and applying the three repairs. Its false-positive list, if
        it produced one, names sentences where the rule over-fires and is
        worth more than the edits.

    the RB2D fill route defect repairs
        Four cherry-pickable items on the superseded 2117 lane: rebase onto
        the merged span API, one 256-byte segment instead of 64, the
        reachable dropped-dispatch hole, and the missing arming case. The
        worktree was detached, so the branch name it pushed to must be read
        from its report or from `git branch -r`.

    test/tool_gate_coverage_and_rs485m_naming
        Five vostro1000-re tools with no Makefile-gated test. Coverage
        first, then naming, because correcting naming without a gate is what
        produced the earlier revert.

## Two findings that must not be lost

The arming gate on the RB2D route could never have armed. `cell_geometry_
unfrozen()` has no case for the fill cell kind, so it falls to
`default: return true`, `nonmaximum_extent` is always set, and every
attended submission refuses. The harm is the ordering: the refusal lands
after the IB is installed and `gpu_routed` is set, so the host fill is
skipped and the ioctl never runs, and the destination is written by
neither. A silicon token spent on that route in its previous state would
have bought an unwritten buffer.

The option ROM's aperture port is not in the image. Of 765 I/O instructions
in the code range, 723 take their port from `DX`; the GPU aperture
assembles that port at run time from a PCI-configuration word saved at
`cs:0x126`, with only its low byte set per access. Immediate search cannot
find these accesses at all, which is why the dataflow facets carry the
recovery.

## First actions on resume

1. Read the ledger, then this file.
2. Fetch all four repositories and list open pull requests and worktrees.
3. For each stranded branch, read what it contains and write a fresh task
   from that, not from the original assignment.
4. Finish 2118, then merge 2119, then the frontier repair, then legacy-2D.

No silicon. No attempt token. Merges, silicon decisions, and token requests
stay with the operator's session and are never delegated.
