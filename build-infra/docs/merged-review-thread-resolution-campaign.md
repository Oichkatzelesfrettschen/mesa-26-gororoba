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
- `review-thread-frontiers/merged-pr93-pr161/` preserves the closed second-batch
  raw denominator and its chronological proof.
- `review-thread-frontiers/merged-thread-frontier-92b67f719e7b/` preserves the
  third-batch raw denominator.  Its manifest binds eleven request/response
  pages, exact query text, default-branch identity, every selected comment,
  hashes, and the chronological stop proof.
- `review-thread-frontiers/merged-thread-frontier-after-QY6A8/` preserves the
  fourth-batch raw denominator.  Its manifest binds twelve request/response
  pages, exact query text, default-branch identity, every selected comment,
  hashes, and the chronological stop proof.
- `review-thread-frontiers/merged-thread-frontier-after-TvpLc/` preserves the
  active fifth-batch raw denominator.  Its manifest binds thirteen
  request/response pages, exact query text, default-branch identity, every
  selected comment, hashes, and the chronological stop proof.
- `review-thread-classifications/merged-thread-frontier-92b67f719e7b/` owns the
  third batch's 50-row assessment, immutable pre-resolution frontier,
  SHA-256-bound mutation journal, generated terminal frontier, and resolution
  ledger.  Keeping these paths batch-scoped preserves the second batch's
  terminal global views without overwriting history.
- `review-thread-classifications/merged-thread-frontier-after-QY6A8/` owns the
  fourth batch's 50-row assessment, immutable pre-resolution frontier,
  SHA-256-bound mutation journal, generated terminal frontier, resolution
  ledger, and append-only evidence-refresh ledger.  A refresh preserves the
  original merge and resolution chronology while binding a later current-main
  re-audit to the preceding evidence commit.
- `review-thread-classifications/merged-thread-frontier-after-TvpLc/` owns the
  fifth batch's 50-row assessment, generated action frontier, and resolution
  ledger.  Its 38 actionable rows require merged repairs before exact-ID
  resolution.
- `merged-review-thread-action-frontier.tsv` and
  `merged-review-thread-resolution-ledger.tsv` remain the stable classified
  views of the closed second batch.  The third and fourth batch terminal views
  remain in their batch-scoped classification directories.
- `../scripts/review_thread_frontier.py` validates the frontier, resolution
  ledger, and evidence-refresh chain offline.  Its unit tests calibrate
  duplicate identity, ordering, false merged evidence, owner-path drift,
  missing closure rows, refresh ancestry, live GraphQL identity, and invalid
  resolution chronology.
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

### Closed second frontier

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

### Closed third frontier

The third batch is anchored to merged `main`
`dd93eca45e58f3592e505b67a15625829feffb91`.  The authenticated private-repo
capture scanned 1,100 merged pull requests across eleven ascending pages and
observed 79 unresolved threads among 2,193 total review threads.

- Rank 1 is `PRRT_kwDOR3YK5M6D9-pq`, created
  `2026-05-22T00:28:15Z` on PR 167.
- Rank 50 is `PRRT_kwDOR3YK5M6QY6A8`, created
  `2026-07-13T12:34:45Z` on PR 1109.
- The last scanned pull request is PR 1130, created
  `2026-07-16T19:18:37Z`.

The pull-request connection is ordered by `createdAt` ascending, and a review
comment cannot predate its pull request.  PR 1130 therefore establishes that
every unscanned pull request was created after the rank-50 comment.  The
retained request cursors prove the eleven pages are contiguous.  Exact query
text, raw responses, normalized selected threads, every selected comment body
and author, and their hashes live in
`review-thread-frontiers/merged-thread-frontier-92b67f719e7b/`.

### Closed fourth frontier

The fourth batch is anchored to merged `main`
`74e17e547d51997ffb074a92b1f74bef56c702a0`.  The authenticated private-repo
capture scanned 1,200 merged pull requests across twelve ascending pages and
observed 99 unresolved threads among 2,454 total review threads.

- Rank 1 is `PRRT_kwDOR3YK5M6QZNKs`, created
  `2026-07-13T12:51:19Z` on PR 1111.
- Rank 50 is `PRRT_kwDOR3YK5M6TvpLc`, created
  `2026-07-25T10:01:25Z` on PR 1193.
- The last scanned pull request is PR 1231, created
  `2026-08-05T01:17:17Z`.

The pull-request connection is ordered by `createdAt` ascending, and a review
comment cannot predate its pull request.  PR 1231 therefore establishes that
every unscanned pull request was created after the rank-50 comment.  The
retained request cursors prove the twelve pages are contiguous.  Exact query
text, raw responses, normalized selected threads, every selected comment body
and author, and their hashes live in
`review-thread-frontiers/merged-thread-frontier-after-QY6A8/`.

### Active captured fifth frontier

The fifth batch is anchored to merged `main`
`63d7eb1226bb4257f3d92cb8c7f4196215c213fb`.  The authenticated owned-origin
capture scanned 1,300 merged pull requests across thirteen ascending pages and
observed 70 unresolved threads among 2,882 total review threads.

- Rank 1 is `PRRT_kwDOR3YK5M6Tw9uC`, created
  `2026-07-25T15:59:22Z` on PR 1194.
- Rank 50 is `PRRT_kwDOR3YK5M6WhUFS`, created
  `2026-08-05T02:01:43Z` on PR 1232.
- The last scanned pull request is PR 1331, created
  `2026-08-11T17:48:32Z`.

The pull-request connection is ordered by `createdAt` ascending, and a review
comment cannot predate its pull request.  PR 1331 therefore establishes that
every unscanned pull request was created after the rank-50 comment.  The
retained request cursors prove the thirteen pages are contiguous.  Exact query
text, raw responses, normalized selected threads, every selected comment body
and author, and their hashes live in
`review-thread-frontiers/merged-thread-frontier-after-TvpLc/`.

## First-batch classification

The source/history audit classifies the 50 rows as:

| State | Threads | Required transition |
| --- | ---: | --- |
| fixed on merged main | 34 | Resolve exact ID; verify; record |
| superseded by merged mechanism | 9 | Resolve exact ID; verify; record |
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

## Closed second-batch classification

`review-thread-classifications/merged-pr93-pr161/assessments.tsv` classifies
the second batch against merged main `0fab98b4e09da3f4d9920535cd2518e2055c9e27`.
`../scripts/review_thread_classification.py` joins those assessments to the
retained GitHub identities and rejects missing, extra, duplicate, reordered,
or state-mismatched rows.

| State | Threads | Required transition |
| --- | ---: | --- |
| fixed on merged main | 48 | Resolve exact ID; verify; record |
| superseded by removed build targets | 2 | Resolve exact ID; verify; record |
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

## Closed classified third-batch frontier

`review-thread-classifications/merged-thread-frontier-92b67f719e7b/assessments.tsv`
classifies the exact third-batch denominator against merged main
`0c8b733b08e32df22cdcc4e19bf4cde411f63f5e`.  The generated
`action-frontier.tsv` preserves the retained chronological order and binds
each row to its discussion URL, current evidence owner, falsifier, and merged
evidence commit.

| State | Threads | Required transition |
| --- | ---: | --- |
| fixed on merged main | 40 | Resolve exact ID; verify; record |
| superseded by a merged mechanism | 10 | Resolve exact ID; verify; record |
| actionable | 0 | None |

The merged repairs cover install locking and status propagation, public source
references, Rust image parity, SFN local-group failure state, package hooks and
deployment consent, VL plane and vertex-row identity, generator-test isolation,
RS480 debugfs parsing, package ownership, result visibility, GPUVis child
failure handling, zscan and IDCT coordinate identity, and Draw output-row
semantics.  Superseded rows govern removed pair and pump targets, retired
install.dat parsing, replaced profiles, a relocated build area, and replaced
package activation contracts.  Static source and Git history establish these
code-state dispositions; runtime, conformance, performance, and silicon claims
remain outside this closure.

All 50 exact thread IDs were resolved after PR 1877 merged.  The resumable
mutation journal records an ordered resolution prefix after every successful
mutation and becomes complete only after the all-ID postflight.  The run began
at `2026-08-26T21:33:23Z`; the final live verification completed at
`2026-08-26T21:34:09Z`.  The terminal frontier and ledger contain 50 closed,
re-verified rows.  The batch registry remains the canonical owner of terminal
campaign state and closure merge identity.

## Closed classified fourth-batch frontier

`review-thread-classifications/merged-thread-frontier-after-QY6A8/assessments.tsv`
classifies the exact fourth-batch denominator against merged main
`e3cc1217ce4b664a3d23e0441cb0e84e4a03b623`.  The generated
`action-frontier.tsv` preserves the retained chronological order and binds
each row to its discussion URL, current evidence owner, falsifier, and merged
evidence commit.

| State | Threads | Required transition |
| --- | ---: | --- |
| fixed on merged main | 35 | Resolve exact ID; verify; record |
| superseded by a merged mechanism | 15 | Resolve exact ID; verify; record |
| actionable | 0 | None |

The merged mechanisms cover CSO teardown ownership, distcc volunteer parsing
and compiler-pair probes, dynamic NIR output spans, opt-scoped Vulkan layers,
Hybrid-TCL and producer-design evidence contracts, source-domain witness
semantics, forced-split shadow admission, owned producer BO staging,
validation-flush reservation, and RS482 stack-manifest payload identity.
Superseded rows govern retired planning vocabulary, replaced typed-gate and
source-domain drafts, the rewritten uploader failure oracle, the deleted
Gallium-backed Vulkan image lane, and the SPDX-only verified-holder policy.
Static source and Git history establish these code-state dispositions;
runtime, conformance, performance, and RS482 silicon claims remain outside
this closure.

All 50 exact thread IDs were resolved after PR 1895 merged.  The resumable
mutation journal records an ordered resolution prefix after every successful
mutation and becomes complete only after the all-ID postflight.  The run began
at `2026-08-27T01:11:19Z`; the final live verification completed at
`2026-08-27T01:12:05Z`.  The terminal frontier and ledger contain 50 closed,
re-verified rows.  The batch registry remains the canonical owner of terminal
campaign state and closure merge identity.

## Classified fifth-batch frontier

`review-thread-classifications/merged-thread-frontier-after-TvpLc/assessments.tsv`
classifies the exact fifth-batch denominator against merged main
`b2b68f46810c9808386f5b7a38adde70352f7050`.  The generated
`action-frontier.tsv` preserves retained chronological order and binds each
row to its exact GraphQL ID, current evidence owner, required observation,
falsifier, and merged evidence commit when one exists.

| State | Threads | Required transition |
| --- | ---: | --- |
| fixed on merged main | 7 | Resolve exact ID after the assessment merges |
| superseded | 5 | Resolve exact ID after the assessment merges |
| actionable | 38 | Implement, test, merge, synchronize, then resolve |

The surviving mechanisms cover a complete headless GL provider and session
workflow; AMD driver-root predicates and artifacts; Hybrid-TCL evidence
separation; CAVLC comment accuracy; stack-manifest dependency and digest
contracts; attribute-isolated external-source materialization; install and
build-control identity; thread-safe Draw options; per-screen R2VB route state;
DRM-shim descriptor, lock, and write-only identity semantics; transactional,
calibrated MPEG-12 surface dumps; Draw semantic provenance; and width-correct
Radeon info queries.

The fixed rows cover component-prefixed guidance history, HBTCL executable
identity, nonempty stack-manifest Build IDs, root-independent source-view
tests, Meson wrap population, and state-token render registration.  Current
SPDX-only and canonical-loader policies supersede five historical requests.
Static source and Git history establish these code-state dispositions;
runtime, conformance, performance, and silicon claims remain outside this
assessment.

All 50 live GraphQL nodes remain unresolved.  Exact-ID mutation begins only
after every actionable mechanism is merged and all rows are reclassified
against synchronized `main`.

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
proof valid.  Authoritative qualification requires a clean worktree whose
checked-out `HEAD` is the declared candidate, so commit the candidate before
running the offline or live publication gate.  Refresh the remote-tracking ref
before relying on that verdict.
The live target binds every exact GraphQL thread ID to its discussion URL,
outdated state, and resolution state.

Static source and Git history establish only code-state facts.  Build, runtime,
conformance, and silicon claims require their respective executed gates and
remain explicitly unclaimed where they were not run.
