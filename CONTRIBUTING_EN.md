# How to contribute

Thank you for wanting to improve the project. This document explains what is
accepted, how to prepare a contribution and how it is reviewed. It is the
translation of `CONTRIBUTING_ES.md`: **the two are updated together**.

## Before you start

- **A single line of development.** The project has one branch, `main`.
  Contributions arrive as pull requests against `main`; there are no release
  branches.
- **Open an issue first** if the change is large (a new module, changes to the
  installer, changes to how the bots behave). That way the approach is agreed
  before you write code that may not fit.
- **License.** Your contribution is published under the project's license, the
  **GNU Affero General Public License v3.0 or later** (`LICENSE`). Patches that
  modify someone else's project keep that project's license (`NOTICE`,
  `REUSE.toml`). By submitting it you confirm you have the right to do so.
- **What is not accepted.** Nothing from Blizzard's World of Warcraft (data, maps,
  icons, client files): it is generated at install time from each player's own
  client. Also no binaries, snapshots of third-party repositories or local data
  (`config.local.sh`, databases, logs).

## Project rules

They are the same ones the maintainer follows; the detail is in `REFERENCES.md`
(«La regla de oro», «Escribir un módulo propio: reglas», «Escribir un parche sobre
código de terceros» — the reference documents are written in Spanish).

1. **Order of preference.** A `config.sh` option if it is enough; a `.sql` in
   `patches/` if it is database rows; a `.patch` only if third-party code has to
   change; an own module in `modules/` for a new feature.
2. **Idempotent SQL.** Each `.sql` deletes its own rows before inserting them.
3. **Patches with a header.** Each `.patch` starts with a license line, the
   problem, the solution and the commit of the third-party project it was written
   against. Check that it applies with `bash tools/verificar-parches.sh`.
4. **Own identifiers** in the ranges listed in `REFERENCES.md` (for example
   `entry` ≥ 600000 for own items and NPCs), so they never clash with the core or
   the modules.
5. **Modules.** Standard AzerothCore structure (`src/*_loader.cpp` and
   `conf/*.conf.dist`); every new option goes into the `.conf.dist` and is set
   from `config.sh`. Work that touches bots on other maps goes in
   `WorldScript::OnUpdate`, never in a `PlayerScript` hook.
6. **License header on own code.** Each new `.sh`, `.py`, `.js`, `.mjs`, `.cpp`,
   `.h` and `.ps1` carries the two SPDX lines the others already have
   (copy the header of any file of the same type: the project's copyright and the `AGPL-3.0-or-later` license).
7. **Unix line endings** in scripts, `.tsv`, `.lock` and `.dist`
   (`.gitattributes`).
8. **Documentation.** There are four main documents: README, INSTALL, REFERENCES
   and CHANGELOG, plus this one. **README, INSTALL and CONTRIBUTING exist in Spanish
   and English and are changed together**: if you touch one, touch its
   translation too. Every change that can be seen from outside gets an entry in
   `CHANGELOG.md`, with a date.
9. **No personal data.** No passwords, keys, real IP addresses, paths on your
   machine, personal names or e-mail addresses in code, documents or commit
   messages. Use neutral examples (`192.168.1.100`, `<path>`). Continuous
   integration checks this with a secret scanner.
10. **New things are tested.** A feature that can be noticed in the game is
    checked with the synthetic client (`tools/cliente-sintetico/`, see below)
    before you ask for review; in the pull request explain how you tested it and
    what came out.

## The synthetic client is not modified

`tools/cliente-sintetico/` is the yardstick used to check that what changes
really works. If a pull request could modify that tool it could make the checks
pass falsely. Therefore:

- **Changes to what already exists there are not accepted**: no deleting or
  editing lines, no renaming or removing files.
- **Pull requests that only add new checks are accepted** (a new case, a new
  file, added lines without touching the others). They go **in a separate pull
  request**, not mixed with other changes, and are verified before being
  accepted: the whole tool is run with and without your addition, and the cases
  that already existed must give exactly the same result.
- Continuous integration automatically rejects a pull request that deletes or
  modifies lines there (`tests/proteger-cliente-sintetico.sh`). That check is an
  aid, not the guarantee: the guarantee is the manual review described below.

## How to prepare the pull request

1. Fork the repository and create a branch from `main`.
2. Change only what is needed for one thing. Several unrelated changes are
   several pull requests.
3. Run what applies to your change before sending it (continuous integration
   repeats these checks):
   - `bash tests/estructura-publica.sh` (structure, licenses, documents),
   - `bash -n` on the scripts you touch and `python3 -m py_compile` on `.py` files,
   - the scripts in `tests/` related to the area you change,
   - `npm test` in `web-panel/` if you touch the panel.
4. Describe in the pull request what problem it solves, how you tested it and
   which documentation you updated.

## How it is reviewed

- **Every pull request is reviewed manually, one by one**, by the maintainer, with
  an AI assistant as a second reader. None is merged automatically, even if
  continuous integration is green.
- Changes in `.github/`, `tests/`, `tools/cliente-sintetico/`, `install.sh`,
  `lib/` and `scripts/` are always reviewed by the maintainer (`CODEOWNERS`).
- **Integration is not a direct merge on GitHub.** Development happens in the
  maintainer's working repository and this public copy is generated from it. An
  accepted pull request is applied there preserving your authorship and shows up
  here in the next publication; the pull request is then closed with a link to
  that publication. It may take a while; it does not mean it was rejected.
- If a change does not fit the rules above, the reason is explained and a way to
  adjust it is suggested. There is no hurry: the project is for personal use and
  is maintained in spare time.
