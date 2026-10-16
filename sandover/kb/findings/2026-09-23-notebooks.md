---
title: Marimo vs Jupyter for local analysis notebooks
date: 2026-09-23
issue: "#15"
workstream: tooling evaluation
status: verified
sha: 76e8eec
---
## Answer (<=5 lines)
marimo (Apache-2.0) is a reasonable pick: notebooks are plain `.py` files that diff like code, the reactive runtime has documented escape hatches (`mo.stop`, run buttons, a lazy "on cell change" mode, and `mo.cache`/`mo.persistent_cache`) for expensive GPU/least-squares cells, it runs sandboxed via `uv`/PEP 723, and it exports to static HTML/Markdown for MkDocs. Jupyter (BSD-3-Clause, JupyterLab + Notebook 7) is still actively maintained but carries the classic `.ipynb` JSON-diff and hidden-state problems, which need nbstripout/jupytext/nbdime bolted on. Both are Windows-supported and can coexist through Jupytext's `py:marimo` format for one-off conversions.

## Picks
| item | license (SPDX) | Windows | last release / last commit | maintenance | verdict | gotcha |
|---|---|---|---|---|---|---|
| marimo (marimo-team/marimo) | Apache-2.0 | supported (win-64 conda build; pip/uv install documented) | v0.24.2 (2026-09-11) / last push 2026-09-23 | active: 22.9k stars, 599 open issues, daily pushes | keep | reactive auto-rerun can restart a multi-minute GPU/fit cell unintentionally unless lazy mode + `mo.stop`/run-button + caching are set up deliberately; some plotting-lib friction (matplotlib `mpl.interactive` broken on Windows, Plotly `.show()` display bugs are open issues) |
| JupyterLab (jupyterlab/jupyterlab) | BSD-3-Clause | supported | v4.6.4 (2026-09-21) / last push 2026-09-21 | active: 15.3k stars, 2600 open issues | defer | `.ipynb` JSON diffs are noisy and hide execution-order state; needs nbstripout/jupytext/nbdime to get clean diffs and reproducibility |
| Jupyter Notebook (jupyter/notebook, v7) | BSD-3-Clause | supported | v7.6.3 (2026-09-21) / last push 2026-09-23 | active: 13.4k stars, 1892 open issues | defer | same JSON/state issues as JupyterLab; kept only as an interop target via Jupytext, not the primary tool |

## Evidence
- marimo stores notebooks as plain `.py` files, git-friendly, executable as a script: https://github.com/marimo-team/marimo
- Reactive execution model (cell reruns propagate to dependents automatically): https://docs.marimo.io/guides/reactivity/
- Expensive-cell guidance: `mo.stop` + `mo.ui.run_button()` to gate expensive calls, lazy "On Cell Change" runtime setting marks dependents stale instead of rerunning, `mo.cache`/`mo.persistent_cache` for memoization: https://docs.marimo.io/guides/expensive_notebooks/
- Runtime configuration reference for the lazy on-cell-change setting: https://docs.marimo.io/guides/configuration/runtime_configuration/
- `marimo.lazy()` defers rendering/computation until viewport visibility or explicit trigger: https://docs.marimo.io/guides/expensive_notebooks/
- PEP 723 inline dependency metadata + `--sandbox` isolates a notebook's deps via uv: https://marimo.io/blog/sandboxed-notebooks
- `uvx marimo edit --sandbox <url>` runs a notebook with uv-managed isolated deps: https://docs.astral.sh/uv/guides/integration/marimo/
- marimo standardizes on anywidget as its plugin API (basis for embedding custom JS/Python widgets similar to Rerun/FiftyOne-style viewers): https://docs.marimo.io/api/inputs/anywidget/
- marimo notebooks/apps can be embedded in other pages via iframe (relevant for coexisting with Gradio-style embedded UIs): https://docs.marimo.io/guides/publishing/embedding/
- Static HTML export: https://docs.marimo.io/guides/exporting/static_html/
- MkDocs publishing guide and dedicated `mkdocs-marimo` plugin for interactive docs: https://docs.marimo.io/guides/publishing/mkdocs/ and https://github.com/marimo-team/mkdocs-marimo
- Known Windows-specific bug: `mpl.interactive` does not work on Windows in marimo: https://github.com/marimo-team/marimo/issues/273
- Known Plotly friction in marimo (KeyError on interaction; `fig.show()` not appearing in app editor): https://github.com/marimo-team/marimo/issues/5326 and https://github.com/marimo-team/marimo/issues/2734
- marimo license/activity (Apache-2.0, 22,879 stars, 599 open issues, pushed 2026-09-23, latest release 0.24.2 on 2026-09-11): https://api.github.com/repos/marimo-team/marimo and https://api.github.com/repos/marimo-team/marimo/releases/latest
- JupyterLab license/activity (BSD-3-Clause, 15,314 stars, 2600 open issues, pushed 2026-09-21, latest release v4.6.4 on 2026-09-21): https://api.github.com/repos/jupyterlab/jupyterlab and https://api.github.com/repos/jupyterlab/jupyterlab/releases/latest
- Jupyter Notebook 7 license/activity (BSD-3-Clause, 13,357 stars, 1892 open issues, pushed 2026-09-23, latest release v7.6.3 on 2026-09-21): https://api.github.com/repos/jupyter/notebook and https://api.github.com/repos/jupyter/notebook/releases/latest
- `.ipynb` JSON diff/hidden-state problem and standard workarounds (nbstripout strips outputs pre-commit; nbdime does structured notebook diff/merge): https://blog.reviewnb.com/rich-diffs-for-jupyter/
- nbstripout as a pre-commit hook for clean diffs: https://theneuralbase.com/jupyter-for-ml/learn/intermediate/nbstripout-for-clean-diffs/
- Jupytext syncs a notebook across `.ipynb`, `.py` (percent), and Markdown text formats, keeping them paired: https://jupytext.readthedocs.io/en/latest/formats-scripts.html
- Jupytext added a `py:marimo` conversion format, delegating ipynb<->marimo.py conversion to marimo itself, enabling coexistence: https://github.com/mwouts/jupytext/blob/main/src/jupytext/marimo.py
- marimo's own `marimo convert notebook.ipynb -o notebook.py` and "Migrate from Jupyter" guide: https://docs.marimo.io/guides/coming_from/jupyter/

## Implications for the design
- Adopt marimo as the default notebook tool for exploratory/analysis work (fits: least-squares/curve-fit diagnostics, offline captured-data iteration per prior "iterate offline first" practice, per-project scratch analysis).
- For any cell doing a multi-minute fit or a GPU pass, explicitly gate it with `mo.ui.run_button()` + `mo.stop`, or switch the notebook's "On Cell Change" runtime setting to lazy, and wrap the computation in `mo.cache`/`mo.persistent_cache` so accidental reactive reruns don't repeat expensive GPU work. Default (eager) reactivity is not safe to leave on for such cells.
- Use `uv run` / `uvx marimo edit --sandbox` with PEP 723 headers per notebook so each notebook's Python deps are pinned and isolated, consistent with the existing uv-managed Python environment.
- Verify matplotlib interactive mode and Plotly `.show()` behavior on the target Windows workstation before relying on them; prefer `mo.mpl.interactive()`/marimo-native plotting entry points and `mo.ui.plotly` where documented as the more reliable Plotly path, and fall back to static image output if issues reproduce locally.
- Rerun's own web viewer and FiftyOne's app are typically launched/embedded as standalone local web servers or iframes; validate embedding them inside a marimo cell via anywidget or an iframe on the actual Windows setup before depending on it — no direct marimo-specific integration guide was found for either.
- Use marimo's static HTML/MkDocs export (`mkdocs-marimo` plugin or `marimo export html`) to publish notebook-derived docs into a local MkDocs site without needing a live Python kernel.
- Keep Jupytext available as the interop path: it can convert one-off `.ipynb` files to/from marimo's `py:marimo` format if a Jupyter-only ipywidget or legacy notebook needs to be brought in, without adopting Jupyter as the primary tool.

## Follow-ups
- Smoke-test Rerun web-viewer and FiftyOne app embedding inside a marimo cell on the target Windows workstation (no documented integration found either way).
- Confirm `mo.mpl.interactive()` / `mo.ui.plotly` avoid the open matplotlib-on-Windows and Plotly display issues in this environment before relying on interactive plots.
- Decide on a per-cell convention (run-button gate vs. lazy runtime vs. `mo.persistent_cache`) for GPU-pass cells so behavior is consistent across notebooks.
