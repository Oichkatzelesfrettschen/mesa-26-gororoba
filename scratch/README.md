# Working scratch

Untracked working files for an in-flight investigation: census output, probe
logs, intermediate tables, and the raw text a finding is later distilled from.
`scratch/.gitignore` keeps everything here out of history except itself and
this file.

Three homes, and the difference is what each one survives:

- `scratch/` holds a working file for as long as the checkout exists. It
  outlives the session that wrote it, which a session-local temporary
  directory does not.
- `docs/hardware/` holds the durable statement a working file becomes once
  its claims are checkable. Program state, contracts, and procedures live
  there and are reviewed like source.
- The `steinmarder-r300` evidence tree holds captures, probe output, and
  sealed bundles. Retained evidence never lands in this repository.

A file that another session needs to read is finished, so it belongs in one
of the latter two.
