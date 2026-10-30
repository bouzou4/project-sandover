# Snapshot run: snapshot sidecar schema

A snapshot run is a play session (`kb/howto/play.md`) where F9 is pressed on anything that
looks wrong. Each press writes two files to `$SANDOVER_DATA/runs/<YYYYMMDD-HHMM>-<slug>/`:
`NNNN.png` (window framebuffer, native resolution, HUD included) and `NNNN.json`:

```json
{
  "index": 3,
  "time_utc": "2026-09-25T18:04:11Z",
  "level": "village1",
  "continue": "village1-hut",
  "jak_pos": [-560712.0, 30924.5, 781004.1],
  "cam_pos": [-548102.3, 51077.0, 760018.9],
  "cam_rows": [[-0.99, 0.0, -0.12], [0.001, 0.99, -0.011], [0.12, -0.011, -0.99]],
  "fov": 64.0,
  "hour": 12.43,
  "mood_weights": [1.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0],
  "toggles": ["(pc-set-bloom 0 1000000)", "..."],
  "build_sha": "c3ba2f6d9",
  "status": "kept",
  "note": null,
  "tags": [],
  "annotations": []
}
```

| Field | Written by | Meaning |
|---|---|---|
| `index` | capture | the `NNNN` of both file names |
| `time_utc` | capture | ISO 8601, UTC, `Z` suffix |
| `level` | capture | level Jak is in (`*target*` current level), null if none |
| `continue` | capture | the game's current continue point (last one reached or warped to), null if none |
| `jak_pos` | capture | Jak's position, game units (4096 per metre) |
| `cam_pos` | capture | `*math-camera*` trans, game units |
| `cam_rows` | capture | `*math-camera*` inv-camera-rot, 3 rows; same convention as `rig-set-camera-row` and the shot manifests' `camera.rot` |
| `fov` | capture | `*math-camera*` fov, degrees |
| `hour` | capture | time of day, 0-24 (fractional) |
| `mood_weights` | capture | the 8 mood palette slot weights of `level` (`mood times[k].w`) |
| `toggles` | capture | the GOAL forms the play script had applied (empty before effects are applied) |
| `hdr` | capture | true when gk also wrote `NNNN.exr` (FP16 target on; `kb/howto/hdr.md`) |
| `build_sha` | capture | engine commit of the staged gk (`build.json`), else the checkout's HEAD |
| `status` | triage | `"kept"` on capture |
| `note` | triage | null on capture |
| `tags` | triage | `[]` on capture |
| `annotations` | triage | `[]` on capture; entries `{id, rect: [x, y, w, h] in native pixels, text, facet, created_utc}` |

The JSON is written atomically (temp file, then rename) once the PNG is complete, so a reader
never sees a half-written sidecar or a sidecar without its image. Capture never rewrites an
existing sidecar; the triage tool owns the last four fields. Numbering after a gk restart
continues past the highest `NNNN` in the run folder and its `deleted/` folder.

Player bug reports (F7, `bug-report.md`) carry a superset of this sidecar; `sandover-bug import`
makes a one-snapshot run folder `runs/bug-<id>/` from one.

`sandover-capture from-snapshot <json>` turns a sidecar into a rig shot at the same level,
camera, Jak position and hour with the same toggles (see `play.md`).

## Vanilla A/B for every annotated frame

Before an annotated frame is judged, re-shoot it with the vanilla look so the complaint can be measured against the original: `sandover-capture from-snapshot <run>/NNNN.json --name NNNN --no-toggles --out-dir <run>/vanilla`. Issues and `summary.md` carry a vanilla column with measured emitter-core and lit-ground RGB in both frames. This lets the complaints about eco and torch brightness be compared with the original.

The rig reuses an existing vanilla frame for the same camera instead of shooting again; see "Vanilla library" in `rig-capture.md` (`--force-vanilla` to override).
