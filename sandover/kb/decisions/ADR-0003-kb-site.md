# ADR-0003: KB site generator
Status: accepted    Date: 2026-09-23    Deciders: project lead

## Context
The KB (`sandover/kb/`) needs a static site build for browsing decisions, findings and
howtos offline. The tooling evaluation covered the Python tooling stack end to end; besides the site
generator, `uv` (env/packages) and `filelock` (build/game locks) were both re-confirmed
as solid picks — see `kb/facts/tools.md` for the full one-line ledger of picks that kept
their default choice without needing their own ADR.

## Options (with cost/risk)
**MkDocs Material (chosen).** MIT, all former Insiders features (Mermaid, rich search,
admonitions, etc.) are now free as of the 2025-11 release. Risk: the project entered
maintenance mode 2025-11-05 and is feature-frozen; security/bugfix support is committed
only through 2026-11, then not guaranteed.
**Zensical.** The same maintainers' (squidfunk) declared successor to MkDocs Material.
Too new to evaluate for this project today; revisit once it has a stable release before
MkDocs Material's Nov 2026 EOL.
**Plain MkDocs / Sphinx / Docusaurus.** Rejected: more setup work for features Material
already ships for free, or (Docusaurus) a Node toolchain this project otherwise avoids.

## Decision
Use MkDocs Material now. All Insiders-tier features needed for this KB (Mermaid,
search, admonitions) are free today, and the site build is fully offline
(`mkdocs build`/`serve`, self-hosted fonts, no CDN calls at build time). Revisit before
the Nov 2026 EOL; the named candidate for that review is **Zensical**, the successor
project from the same maintainers.

## Consequences
- No deep custom plugins against MkDocs Material — it will not receive new features.
- `mkdocs.yml` must disable remote font/CDN fetches to keep `mkdocs serve` fully offline.
- Calendar reminder implicit in this ADR: re-check Zensical's status before 2026-11.
- Source: `kb/findings/2026-09-23-python-infra.md`.
