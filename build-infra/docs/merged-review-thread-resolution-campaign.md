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
  closed fifth-batch raw denominator.  Its manifest binds thirteen
  request/response pages, exact query text, default-branch identity, every
  selected comment, hashes, and the chronological stop proof.
- `review-thread-frontiers/merged-thread-frontier-after-WhUFS/` preserves the
  active sixth-batch raw denominator.  Its manifest binds sixteen pull-request
  pages, seven comment-continuation pages, exact query text, default-branch
  identity, every selected comment, hashes, and the chronological stop proof.
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
  fifth batch's 50-row assessment, immutable pre-resolution frontier, ordered
  recovery journal, generated action frontier, resolution ledger, and
  append-only evidence-refresh ledger.  All 50 exact rows are closed and
  re-verified.
- `merged-review-thread-action-frontier.tsv` and
  `merged-review-thread-resolution-ledger.tsv` remain the stable classified
  views of the closed second batch.  The third, fourth, and fifth batch
  terminal views remain in their batch-scoped classification directories.
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

### Closed fifth frontier

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

### Active captured sixth frontier

The sixth batch is anchored to merged `main`
`47135b3e0a18caa2f5ca8d0ed46c5abc245a5429`.  The authenticated owned-origin
capture scanned 1,600 merged pull requests across sixteen ascending pages and
observed 128 unresolved threads among 3,264 total review threads before the
chronological stop.

- Rank 1 is `PRRT_kwDOR3YK5M6WhUFW`, created
  `2026-08-05T02:01:43Z` on PR 1232.
- Rank 50 is `PRRT_kwDOR3YK5M6ZIj8L`, created
  `2026-08-14T01:24:56Z` on PR 1593.
- The last scanned pull request is PR 1632, created
  `2026-08-17T04:59:47Z`.

The pull-request connection is ordered by `createdAt` ascending, and a review
comment cannot predate its pull request.  PR 1632 therefore establishes that
every unscanned pull request was created after the rank-50 comment.  Retained
request cursors prove that the sixteen pull-request pages and seven continued
comment pages are contiguous.  Exact query text, raw responses, normalized
selected threads, every selected comment body and author, and their hashes
live in `review-thread-frontiers/merged-thread-frontier-after-WhUFS/`.

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

The v2 recovery journal records an ordered frontier subsequence when exact
threads were resolved independently before the batch-wide mutation.  Each
entry carries its own merged commit, pull request, merge time, resolution
observation, and second live verification.  The `record` command verifies the
pull request merge, commit ancestry, canonical-target parity with `main`, the
retained discussion URL, and two resolved-state queries.  It never mutates a
GitHub thread.  The fiftieth entry becomes complete only after one additional
query proves all 50 exact IDs resolved together.  Offline tests exercise the
journal and ledger invariants; live network observations remain confined to
the command itself.

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
initially classified the exact fifth-batch denominator against merged main
`b2b68f46810c9808386f5b7a38adde70352f7050`; later row transitions retain
their newer merged evidence commits explicitly.  The generated
`action-frontier.tsv` preserves retained chronological order and binds each
row to its exact GraphQL ID, current evidence owner, required observation,
falsifier, and merged evidence commit when one exists.

| State | Threads | Required transition |
| --- | ---: | --- |
| actionable | 0 | None |
| fixed on merged main | 0 | None |
| closed as fixed | 45 | Retain exact merge and post-resolution evidence |
| closed as superseded | 5 | Retain exact merge and post-resolution evidence |

The uid-0 source-root replay at merged main
`59593fda660a7603789c895c479c2bb38d5140fa` uses:

```sh
sudo -A env PYTHONDONTWRITEBYTECODE=1 PYTEST_DISABLE_PLUGIN_AUTOLOAD=1 \
  python3 -m pytest -q \
  build-infra/tests/test_source_root_control.py \
  build-infra/tests/test_mount_boundary_calibration.py
```

The replay passes all 125 tests.  Shared fixture namespaces let the six
affected cases reach their bare-repository and source-view assertions while
the production root-account rejection retains its exact diagnostic.  PR 1918
binds both fixes to merged main, and both exact GitHub threads are resolved and
re-queried.

The ordered recovery journal records independently merged repairs.  PR 1912
binds raw-alias backing identity to merge commit
`400df9e74aeacc6917410a887a3bf33bf5fda167`; PR 1900 binds the preload-test
SPDX header to `4b0c1f55abd1c9d84022973e14fd40d639426ec9`; and PR 1913 binds large-lock
layout normalization to `05023afc27fcc9fb0637ecacfa425d9fc4263a81`.  PR 1916
binds ten re-audited fixed or superseded rows to merge commit
`a2bfd6032c81faa75994dfd4f57b279200a0776f`.  PR 1918 binds the source-root
fixture isolation repairs to `59593fda660a7603789c895c479c2bb38d5140fa` and
re-audits the Meson-wrap finalization evidence at the same merged commit.
PR 1920 binds the MPEG-12 dump namespace, transactional-write verdict, and
calibration rows to `2721b9314aed7d55615d5fbf4f8247b4d0fe84fc` after clean
Clang, Valgrind, MinGW, mutation, and merged-main qualification.  Four review
threads created on PR 1920 are outside the retained fifth-batch denominator;
their MC entrypoint, random namespace, Windows path, and anchored-root repairs
are present at the same merge commit, and all four exact IDs are resolved and
re-queried.
PR 1922 binds the eight unresolved headless-runner rows to
`20286a21d04942790561bc931ea332eb59b6214b`.  The lifecycle repair gives each
Xorg run a persisted 128-bit systemd unit name and requires its live
`InvocationID` and `MainPID` before systemd stops the unit.  Ten calibrated
paths cover inactive and removed units, PID reuse, replacement invocations,
failed state, malformed identity, timeout, and direct-signal regression.  The
complete headless group is resolved and re-queried after merged-main
qualification.
All 50 recorded GitHub threads are resolved and re-queried.  The journal is
complete, and its batch-wide postflight records the final verification time.

Eight re-audited rows are closed with exact-ID resolution evidence.  The
current guidance uses the fork-specific AMD Vulkan scope.  PR 1902's unchanged
driver-root audit passes all 67 calibrations and the live three-root check.
PR 1903's unchanged HBTCL document separates the unsignaled VAP/GA stall from
completed stale-US draws and binds demand to the hash-verified 108-cell census.
PR 1904's unchanged CAVLC comments name the independent oracle and exact coefficient
index bounds; the focused block test passes from the retained qualification
build.

Two RS482 stack-manifest rows are fixed by PR 1905 merge commit
`82b7f3491b25b02063d31952b566c873ca5b2d8e`.  A clean Python 3.14 environment
installs the pinned `jsonschema` 4.26.0 requirement and passes all four schema
tests.  The shared identity definitions and mutation corpus reject short,
long, and final-newline SHA-256 and Git-object strings.  PR 1925 re-audits the
unchanged merged-main targets at merge commit
`34cfba8dc91dd24f52649e6a6ea19f1299836d17`; both exact GitHub threads are
resolved and re-queried.

Two build-infrastructure rows are fixed on merged main.  PR 1906 merge commit
`3980f87045a5a6cbb856b9f299cb52eaf8b29415` delegates mount-calibration root
selection to `tempfile` and hashes physical tracked bytes without clean
filters.  The non-default `TMPDIR` and hostile-filter fixtures pass.  PR 1918
requalifies the expanded source-root files at merge commit
`59593fda660a7603789c895c479c2bb38d5140fa`; all 125 source-root and mount tests
pass.  PR 1927 re-audits both targets at merge commit
`c249fec76a8579d842cc69e4182be62b3cbe01bd`; both exact GitHub threads are
resolved and re-queried.

Five Draw rows are fixed on merged main.  PR 1907 merge commit
`f7f60db090af11e7ad491042afccd29e40fc6b2e` initializes every NIR decline with
a nonnull fallback.  PR 1908 merge commit
`e7656d6bf70d3492a70200cbfb97f8fb646b7c58` moves both Draw NIR options onto
the atomic once-option mechanism, calibrates 16-thread first use, and records
the production caller and r300 prefilter searches.  PR 1929 merge commit
`60c34e778d475469fae20e0714de79464bc42136` names all seven direct NIR scan
sites and states the separate VAR-to-GENERIC and TEX-to-TEXCOORD mappings.
The focused Clang 22 build and output-location oracle pass at the repair SHA.
PR 1930 re-audits all five targets at merge commit
`9651cd7102e5e3ec93296c5265535ed00fe27172`; all five exact GitHub threads are
resolved and re-queried.

Four rows are fixed on merged main.  PR 1900 installs the positive double-hyphen
rule, and PR 1901 merge commit
`bc38bb8840c92d4b64d9f25d5d83c5f2345007c7` is its unchanged evidence owner.
PR 1909 isolates source archives from repository-local attributes.  PR 1910
holds identities provisional until installation succeeds.  PR 1911 merge
commit `767de3c3f4dda9cef71d83fa3076d90561cd5f61` records all five build controls.
PR 1918 merge commit `59593fda660a7603789c895c479c2bb38d5140fa` is the
unchanged archive source-and-test owner.  PR 1922 merge commit
`20286a21d04942790561bc931ea332eb59b6214b` is the unchanged Makefile owner.
All 125 source-root tests and the complete source-root selection integration
fixture pass.  PR 1932 re-audits all four targets at merge commit
`b918e1135f318e3253a3fc76cea94bdbbae6c560`; all four exact GitHub threads are
resolved and re-queried.

The final three mechanisms are fixed on merged main and closed.  PR 1934 merge commit
`3b355908198285831108be57003bdfbd69a796cb` stores the RS480 standing-route
composite in screen-owned state and covers both mixed-screen creation orders
plus alternating consumers.  PR 1936 merge commit
`b058622f9f395c151befc9c0294230cd383815d6` classifies all 39 current
DRM_RADEON_INFO requests by kernel ABI layout and routes them through typed
u32, u64, or exact-array wrappers with pre-ioctl validation.  PR 1937 merge
commit `7cc2fad09eac2f38dd1cbec2a26f32c7a1366f1b` preserves O_WRONLY while
validating the same state-token device and inode through a readable witness;
direct opens, inherited exec, SCM_RIGHTS transfer, wrong-inode rejection,
descriptor closure, and registry cleanup pass.  The evidence-refresh ledger
re-audits the unchanged state-token registration, raw-alias backing identity,
preload-test SPDX header, and large-lock normalization mechanisms at the same
DRM-shim merge commit.

The closed fixed rows cover component-prefixed guidance history, HBTCL
executable identity, nonempty stack-manifest Build IDs, Meson wrap population,
root-independent source-view fixtures, state-token render registration, and
the three DRM-shim repairs.  They also cover the target-bound headless GL
provider and run-scoped Xorg lifecycle, random MPEG-12 dump namespaces,
transactional failure verdicts, and calibrated stage-boundary payloads.
SPDX-only, canonical-loader, merge-subject, and emoji-scope policies supersede
five historical requests.
Static source and Git history establish these code-state dispositions;
runtime, conformance, performance, and silicon claims remain outside this
assessment.

Live GraphQL state reports all 50 exact IDs resolved.  The three final repair
commits are ancestors of `origin/main`, their declared target files match the
current candidate, and the terminal journal binds each resolution to its
repair PR and post-resolution verification time.

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
