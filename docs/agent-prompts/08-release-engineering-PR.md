# chore(release): release engineering (task R4)

Branch `chore/release-1.0` off `master`.

## What shipped

| Piece | Where |
| --- | --- |
| The version, in one place | `project(haiku_daw VERSION 1.0.0 LANGUAGES CXX)` generates `Version.h` (and the app resource) into the build tree; the About box, the startup line, `--version` and the `.rdef` all read it. |
| About box | Help ▸ About — a BAlert with the name, the version and one line about the app; `--version` answers without a window, so a packager or a bug report can ask. |
| App resource | `haiku-daw.rdef.in` → generated rdef → `rc` + `xres` attach the signature and version to the binary (POST_BUILD, only where those tools exist). No icon yet; the rdef says where it goes. |
| README | What it is, what it needs (Haiku R1/beta5+, CMake 3.16+, optional lilv/lv2), how to build and test, and its limitations stated plainly: WAV only, no VST, the dynamic-latency caveat, no audio in the dev VM. |
| CHANGELOG | The 1.0 feature set, by area. |
| LICENSE | MIT, on `master` since 2026-10-09 (the repo went public without one). |
| HaikuPorts recipe | `haiku-daw-1.0.0.recipe` — a draft by necessity: the checksum needs the tagged tarball, so it is a marked TODO rather than an invented number. |
| User guide | `docs/USER_GUIDE.md` — the tour, not the manual; only behaviour that is in the code, and the in-app shortcuts window is the key list of record. |
| Stale docs | HANDOFF's dev loop (GitHub + `scripts/vm.sh`, R1/beta6, SSH works), PRODUCTION_HANDOFF's frozen counts, 03's bundle recipe, ROADMAP's Phase I (shipped), 07's DAF naming, and the session entry's two wrong claims (app_server reachability, "there is no remote"). |

## Verified

- Host build + suite green; the cross-compile check learned about generated
  headers so it still covers `main.cpp` and `MainWindow.cpp`.
- On the VM: `daw --version` prints the generated string, the build's POST_BUILD
  resource step runs (`rc` + `xres` are in the base system), and the suite —
  including `ui_functional_tests`, which now asserts the About box appears and
  the version is the generated one — is green. Counts in the PR for the merge.

## Not done

- **The release tag and the recipe's checksum.** Both belong to the moment Marc
  cuts 1.0; the recipe's checksum cannot exist before the tag does.
- **An application icon.** The rdef has the placeholder comment; an HVIF belongs
  in it once there is artwork.
- **The `.hpkg` build itself** was not run here: HaikuPorts' tooling builds the
  recipe, and that is the packager's (or Marc's) run, on the tag.
