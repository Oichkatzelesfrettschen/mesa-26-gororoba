# Resolved review thread merge evidence

This record binds each exact GitHub thread that was resolved at capture to the
reviewed path, the source change that answered or superseded the comment, and
the latest merged owner of the surviving target at Mesa revision
`ba1652a87e55351b40743b79f0bec9dee44a9f6b`.

`frontier.tsv` is the captured set. `resolution-evidence.tsv` retains the
GitHub thread URL, closure state, resolution source, merge identity, current
target blob, current source owner, change subjects, and PR titles.
`evidence-overrides.tsv` covers paths that moved, were renamed, were removed,
or were resolved through the documented direct-main change. The tool queries
GitHub again for every captured thread and validates override PR metadata
against GitHub.

Run `make -C build-infra review-thread-reconciliation-check` to replay the
record. The check confirms closure state, source ancestry, and the recorded
source identity. It does not claim a new runtime, conformance, or silicon test
run for every historical review comment; those evidence classes remain limited
to the specific source changes and tests named by their merged pull requests.
