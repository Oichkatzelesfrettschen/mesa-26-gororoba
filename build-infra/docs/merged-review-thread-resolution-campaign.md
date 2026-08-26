# Merged Review Thread Resolution Campaign

This campaign closes merged-pull-request review debt through current-main
evidence, corrective integration, and exact GitHub thread verification.  A
merged pull request, an outdated diff anchor, or a local patch does not close a
review claim.  Closure requires the governing mechanism on merged `main`, the
relevant focused gate, resolution of the exact GraphQL thread ID, and a final
`isResolved=true` re-query recorded in the resolution ledger.

## Canonical artifacts

- `merged-review-thread-batch-registry.tsv` owns batch identity, state, commit
  anchors, exact endpoint thread IDs, and paths to retained artifacts.
- `review-thread-frontiers/merged-pr1-pr90/` preserves the first classified
  frontier and its 50-row post-resolution ledger.
- `review-thread-frontiers/merged-pr93-pr161/` is the active second-batch raw
  denominator.  Its manifest binds two request/response pages, exact query
  text, default-branch identity, every selected comment, hashes, and the
  chronological stop proof.
- `merged-review-thread-action-frontier.tsv` and
  `merged-review-thread-resolution-ledger.tsv` remain the stable classified
  views.  They retain the last fully classified batch until the second-batch
  source audit produces its action rows.
- `../scripts/review_thread_frontier.py` validates the frontier and ledger
  offline.  Its unit tests calibrate duplicate identity, ordering, false merged
  evidence, owner-path drift, missing closure rows, live GraphQL identity, and
  invalid resolution chronology.
- `../scripts/review_thread_batch_capture.py` captures and replays a bounded
  oldest-unresolved denominator.  Its checker rejects file-membership, hash,
  exact-query, request-cursor, selected-comment, and chronology mutations.

The historical `last-100-pr-review-comment-audit.md` records a different
archive operation.  It resolved 143 GitHub threads while only 11 findings were
addressed in that working tree.  Its status remains historical evidence and
does not satisfy this campaign's closure gate.

## Oldest-thread denominator

### Closed classified frontier

The first batch is anchored to merged `main`
`7ac62205eb882df42462d849cc549086b59227ea` and ordered by the first review
comment's `createdAt`, then GraphQL thread ID.

- The first 100 merged PRs contained 60 unresolved threads.
- Rank 50 is `PRRT_kwDOR3YK5M6Cvkag`, created
  `2026-05-18T06:59:23Z` on PR 90.
- The last PR in that first page was created `2026-05-19T05:55:23Z` and the
  next page begins `2026-05-19T07:04:51Z`.

A review thread cannot predate its pull request.  Therefore no later PR page
can contain a thread older than rank 50, and the 50-row batch is a finite
chronological denominator.

The capture query uses `pullRequests(first:100, states:MERGED,
orderBy:{field:CREATED_AT,direction:ASC})`, then requests
`reviewThreads(first:100)` with `id`, `isResolved`, `isOutdated`, `path`,
`originalLine`, and the first comment's `createdAt`, author, body, URL, and
commit OIDs.  The normalization filters `isResolved=false`, sorts by
`(comment.createdAt, thread.id)`, and selects the first 50 rows.  The first
page contained no PR with more than 100 review threads, so neither nested
connection was truncated.

### Active captured frontier

The second batch is anchored to merged `main`
`661a73bf5bb99146ce096cc086613c7a819895f3`.  The authenticated private-repo
capture scanned 200 merged pull requests across two ascending pages and
observed 65 unresolved threads among 322 total review threads.

- Rank 1 is `PRRT_kwDOR3YK5M6C8Exz`, created
  `2026-05-18T19:03:09Z` on PR 93.
- Rank 50 is `PRRT_kwDOR3YK5M6D9CKy`, created
  `2026-05-21T22:37:33Z` on PR 161.
- The last scanned pull request is PR 209, created
  `2026-05-23T07:15:36Z`.

The pull-request connection is ordered by `createdAt` ascending, and a review
comment cannot predate its pull request.  PR 209 therefore establishes that
every unscanned pull request was created after the rank-50 comment.  The
retained request cursors prove the two pages are contiguous.  Exact query text,
raw responses, normalized selected threads, every selected comment body and
author, and their hashes live in
`review-thread-frontiers/merged-pr93-pr161/`.

## First-batch classification

The source/history audit classifies the 50 rows as:

| State | Threads | Required transition |
| --- | ---: | --- |
| fixed on merged main | 34 | Re-query, resolve exact ID, re-query, record ledger |
| superseded by merged mechanism | 9 | Re-query, resolve exact ID, re-query, record ledger |
| actionable | 7 | Implement, test, merge, synchronize, then resolve |

The seven actionable rows collapse into five mechanism-coherent changes:

1. Wide-phi admission: require exact opt-in and bind selector discovery to the
   phi's actual index provenance.
2. Indirect draw addressing: keep Vulkan-buffer-relative bounds separate from
   BO-relative packet offsets for nonzero memory-bind offsets.
3. Carrier submit scratch ownership: replace loop-accumulating `alloca`
   storage with reusable owned memory and stress repeated small IBs.
4. Robustness metadata identity and order: establish one producer/consumer
   index domain and make descriptor-set/pipeline binding order equivalent.
5. TG4 swizzle metadata identity: separate shader-stage state and cover every
   legal sampled-image resource ID, including dynamic arrays and later sets.

These changes remain separate reviewable pull requests.  The 50-thread batch
is the audit denominator, not permission to combine unrelated code paths.

## Active classification

`review-thread-classifications/merged-pr93-pr161/assessments.tsv` classifies
the second batch against merged main `0fab98b4e09da3f4d9920535cd2518e2055c9e27`.
`../scripts/review_thread_classification.py` joins those assessments to the
retained GitHub identities and rejects missing, extra, duplicate, reordered,
or state-mismatched rows.

| State | Threads | Required transition |
| --- | ---: | --- |
| fixed on merged main | 48 | Re-query, resolve exact ID, re-query, record ledger |
| superseded by removed build targets | 2 | Re-query, resolve exact ID, re-query, record ledger |
| actionable | 0 | None |

The two findings that survived the initial source audit both governed the Palm
cube-gather comment.  PR 1866 merged the complete correction: Palm is the
validated code path, Cypress/Juniper/Redwood/Cedar are Evergreen GPUs, and
Cayman is Northern Islands.  Static source and Git history support these
dispositions.  Runtime, conformance, performance, and silicon claims remain
outside this classification.

All 50 exact thread IDs were resolved after PR 1867 merged.  The resumable
mutation journal records an ordered resolution prefix after every successful
mutation and becomes complete only after the all-ID postflight.  The run began
at `2026-08-26T20:35:01Z`; the final live verification completed at
`2026-08-26T20:35:53Z`.  The active frontier and ledger now contain 50 closed,
re-verified rows.  The batch registry remains
the canonical owner of terminal campaign state and closure merge identity.

`../scripts/review_thread_resolution.py` owns preflight, resumable exact-ID
mutation, postflight, journal replay, and deterministic ledger generation.
Its journal binds the immutable pre-resolution frontier hash so later closed
state cannot rewrite the mutation denominator.

## Verification

Run the offline contract with:

```sh
make -C build-infra review-thread-frontier-check
make -C build-infra review-thread-frontier-live-check
make -C build-infra review-thread-frontier-unit-test
```

Capture a subsequent denominator into a new mechanism-named directory with:

```sh
python3 build-infra/scripts/review_thread_batch_capture.py capture \
  --batch-id merged-review-thread-closure-batch-NNNN \
  --batch-size 50 \
  --output-dir build-infra/docs/review-thread-frontiers/merged-prFIRST-prLAST
```

The output directory must be empty.  Run the checker before classification:

```sh
python3 build-infra/scripts/review_thread_batch_capture.py check \
  --input-dir build-infra/docs/review-thread-frontiers/merged-prFIRST-prLAST
```

Immediately before resolving a thread, query its exact node ID and confirm it
is still unresolved.  Immediately afterward, query the same ID and require
`isResolved=true`.  Record the merged evidence commit, evidence PR, merge time,
resolution time, verification time, and mechanism note in the ledger.
The offline validator requires every declared evidence commit to be an ancestor
of `origin/main` and requires each semicolon-delimited `canonical_data_target`
to match between that evidence commit and candidate `HEAD`.  A target may name
a complete path blob or an inclusive source slice such as
`build-infra/Makefile#L121-L126` when one large control file owns unrelated
mechanisms.  A change to the declared owner content therefore forces re-audit
and an evidence-commit refresh, while unrelated candidate changes leave the
proof valid.  Refresh the remote-tracking ref before relying on that verdict.
The live target binds every exact GraphQL thread ID to its discussion URL,
outdated state, and resolution state.

Static source and Git history establish only code-state facts.  Build, runtime,
conformance, and silicon claims require their respective executed gates and
remain explicitly unclaimed where they were not run.
