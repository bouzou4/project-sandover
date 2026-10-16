# Notify

`task -t sandover/Taskfile.yml notify TITLE="..." BODY="..."` posts
`{title, body, type: "info"}` as JSON to `$SANDOVER_NOTIFY_URL`.

`SANDOVER_NOTIFY_URL` is a **user-set environment variable**, not committed anywhere and
never set by tooling. Set it yourself (e.g. in your shell profile or
`scripts/tasks/.env`, which is gitignored) if you want notifications; if it's unset, the
task is a no-op.

Use this only to ping for human intervention (a decision, an approval, something blocked),
never for routine progress updates.
