# Haiku conformance audit

What Haiku's own published requirements ask of an application, checked item by
item against this repository, with every gap routed to the task that should fix
it. **This document changes no code.** It exists so that "does our app meet
Haiku's requirements?" has an answer with evidence and owners instead of an
opinion.

Audited: `master` at `6d88063`. Every claim is `file:line` on that commit, or a
path for things that are not code. Where a deviation is already fixed on one of
the unmerged branches, the branch is named — those fixes are not to be redone.

## Sources

| Source | URL |
|---|---|
| Development (index) | https://www.haiku-os.org/development/ |
| Haiku Coding Guidelines | https://www.haiku-os.org/development/coding-guidelines |
| Human Interface Guidelines (HIG) | https://www.haiku-os.org/docs/HIG |
| Prepare for Publishing | https://www.haiku-os.org/development/apps-prepare |
| Icon Guidelines | https://www.haiku-os.org/development/icon-guidelines |

The HIG is one document in twelve chapters; citations below are
`<chapter>, <section>` with the `#chapterN` anchor.

## Scope, and how to read this

Haiku publishes two kinds of rule and they do not have the same force here:

**Rules that apply to any application on Haiku** — the HIG, the app
conventions it describes, packaging, icons, command-line switches. **These bind
us.** Parts A–L and N–O below.

**Rules that apply when contributing code to Haiku itself** — the Coding
Guidelines. Their own preamble says so: *"If you will be contributing code or
patches to Haiku, you will need to strictly follow the code style guidelines.
Code which doesn't follow the guidelines below will not be accepted."* They are
about the Haiku tree, and the same document's first rule is *"Make your code
not stick out — make it consistent with the rest of the code you are
contributing to"*, which for files inside this repository means this
repository's style. Part M is therefore marked **upstream only** — it is a list
of what would have to change *if this code were ever offered to Haiku*, not a
defect list. It is not counted with the app-facing findings.

**Where our own rules differ, ours win.** `docs/UI_GUIDELINES.md` §5 records
Marc's decision that the app never writes system settings and that the default
look is native Haiku with an optional dark mode; where that differed from an
earlier reading of the HIG, it is noted rather than reported as a defect. No
item below asks to reopen a decision recorded in `docs/UI_GUIDELINES.md`,
`docs/PLAN_1.0.md` or `docs/agent-prompts/24-continue-1.0-ENTRY.md` §3.

**Evidence is static.** This audit was produced without running the app (a
machine rule for the task that produced it). Anything that can only be settled
by looking at the screen — tab order, label fit, control spacing — is marked as
such inside the item and belongs to the `DAW_UI_SHOTS` pass that
`docs/UI_GUIDELINES.md` §1 already requires. No item was marked Met on the
strength of a test that could not see the screen.

**Statuses**

- **Met** — the requirement is satisfied; evidence given.
- **Deviates** — not satisfied. Severity and the owning task follow.
- **Not applicable** — the requirement addresses something this app does not
  do (no printing, no replicants, no find window, no icon yet).

**Severities** — **High**: a user-visible defect or a data-safety matter.
**Medium**: a user-visible deviation from Haiku behaviour that a reviewer would
notice. **Low**: cosmetic or internal, cheap to fix.

**Routing tasks** — `T12` HVIF icons, `T13` M8 release/packaging, `T6` M3
preferences work, `T4` M2 arrange-window feel, `Marc's decision` (a product
choice), `upstream only` (see above), or a task this audit proposes — **T14**
and **T15**, written up at the end in the `§5` style of
`docs/agent-prompts/24-continue-1.0-ENTRY.md` so Marc can adopt, renumber or
reject them.

## Summary

**92 items: 45 Met, 35 Deviates, 12 Not applicable.**

| Part | Met | Deviates | N/A | Total |
|---|---|---|---|---|
| A. Development index | 2 | 1 | 0 | 3 |
| B. HIG 2 — Qualities of good software | 6 | 0 | 0 | 6 |
| C. HIG 3 — Conventions of Haiku | 7 | 5 | 2 | 14 |
| D. HIG 4 — Getting input | 3 | 2 | 1 | 6 |
| E. HIG 5 — Drag & drop, clipboard | 1 | 2 | 1 | 4 |
| F. HIG 6 — Use of text | 3 | 4 | 0 | 7 |
| G. HIG 7 — Branding | 1 | 2 | 0 | 3 |
| H. HIG 8 — Cursors | 1 | 1 | 1 | 3 |
| I. HIG 9 — Menus | 3 | 5 | 1 | 9 |
| J. HIG 10 — Windows | 6 | 3 | 0 | 9 |
| K. HIG 11 — Special purpose windows | 3 | 1 | 2 | 6 |
| L. HIG 12 — Controls | 8 | 1 | 1 | 10 |
| M. Coding Guidelines *(upstream only)* | 1 | 6 | 1 | 8 |
| N. Prepare for publishing | 0 | 2 | 0 | 2 |
| O. Icon Guidelines | 0 | 0 | 2 | 2 |
| **Total** | **45** | **35** | **12** | **92** |

Of the 35 deviations, **29 face the application** and **6 are upstream-only**
(Part M). The five worst, in order:

1. **C-7 (Medium)** — `src/main.cpp:129-148` rewrites the *user's*
   system colours on every launch. Fixed on `feature/theme-modes` (T1).
2. **I-8 (Medium)** — there is no way to open the manual from the app; the
   Help menu holds only About. `docs/USER_GUIDE.md` ships and nothing reads it.
3. **I-3 (Medium)** — no Program menu; About is in Help and Quit is in
   File, against the HIG's menu map.
4. **G-2 (Medium)** — the About box has the name and version but no author
   and no copyright, its button says OK rather than Close, and nothing handles
   Cmd+W.
5. **H-1 (Medium)** — `src/ui/InstrumentWindow.cpp:156-160` pops a busy
   cursor, which the HIG forbids outright.

Plus two notes. No item is **High**: the audit found no user-visible defect that
loses data or breaks the app, and the two data-safety-adjacent findings
(K-4's destructive default button, C-7's system-colour write) are Medium and
Medium respectively — the first because it is one button order, the second
because T1 already fixes it. And the publication gaps (N-1, N-2) are not
oversights: they are what **T12** and **T13** exist for.

---

# Part A — Development index

**A-1. "Next, become familiar with the Haiku API."**
<https://www.haiku-os.org/development/>

**Status: Met.** The app is built on the kits rather than ported around them:
`BApplication`/`BWindow`/Layout Kit (`src/main.cpp:112`, `src/ui/MainWindow.cpp:280`),
Media Kit via `BSoundPlayer` (`src/ui/TransportController.cpp`), Midi Kit 2,
Storage Kit (`BFilePanel`, `BQuery`, `BNode` attributes), Tracker integration
(`src/main.cpp:97-110`, `:185-192`, `:206-222`). No third-party UI toolkit.

**A-2. "You should learn to apply common Haiku patterns in your
applications."**

**Status: Met.** The patterns the same page points at are present:
`find_directory` for every path (`src/main.cpp:74`, `src/ui/ProjectDocument.cpp:69`),
`B_SINGLE_LAUNCH` and a `file_types` resource (`haiku-daw.rdef.in:14`, `:18-20`),
a MIME type installed with a sniffer rule (`src/main.cpp:97-110`), the Tracker
open/save panels, and a flattened settings file in the user settings folder.
Record: `docs/agent-prompts/12-tracker-integration-PR.md`.

**A-3. "Once you have a working application you should prepare your app for
publication."**

**Status: Deviates — Low.** The page links two preparation steps, and neither
is done: *"Translating your application using catkeys files"* and *"Create
icons for your application"*. No `catkeys`/Locale Kit use exists anywhere
(`grep -rn "BCatalog\|B_TRANSLATE" src/` → nothing), and the resource file
ships no icon. Evidence and routing: **N-1**, **N-2**, **O-1**, **O-2**.

---

# Part B — HIG 2, Qualities of good software

**B-1. "Good software keep the lines of communication open."** (2, *Good
Software Gives Plenty of Feedback*)

**Status: Met.** Export renders on a worker thread behind a
`BStatusBar` you can cancel (`src/ui/ExportProgressWindow.cpp:23`, `:29-31`);
recording, playback and meter state are visible continuously
(`src/ui/TransportBar`, `src/ui/MeterView.cpp`); the title carries the dirty
marker (`src/ui/ProjectDocument.cpp`, `Title()`).

**B-2. "make it tough for the user to make a mistake … Build constraints into
your application which prevent errors."** (2, *Good Software Makes Errors
Hard*)

**Status: Met.** Takes never overwrite: names land on the first free
`<name>-N.wav` and `WavWriter` opens `O_CREAT|O_EXCL`
(`docs/agent-prompts/11-unique-takes-PR.md`). A recovery copy guards a crash
(`src/ui/MainWindow.cpp:1439-1445`). Missing media is offered Locate… before
the session proceeds (`src/ui/MainWindow.cpp:2400-2432`).

**B-3. "Error messages … need to describe at the user's level of expertise
what happened and suggest what the user can do to remedy the situation."**
(2, *Good Applications Handle Errors Gracefully*)

**Status: Met.** One `ReportError` helper carries every reported failure
(`src/ui/MainWindow.cpp:2389-2395`); the missing-media dialog lists what is
gone and offers a way out (`:2426-2432`). Error-message *content* is audited
separately at F-1.

**B-4. "Make commands undoable whenever possible and when it is not possible,
be sure to inform the user that such is the case."** (2, *Good Software is
Forgiving*)

**Status: Met.** Every model edit goes through `CommandStack` (a project hard
rule); M0.5 closed the four paths that bypassed it
(`docs/agent-prompts/15-undo-gaps-PR.md`).

**B-5. "Good programs use language appropriate for the audience."** (2, *Good
Software Uses Everyday Language*)

**Status: Met.** Alert copy is plain: "This project has unsaved changes."
(`src/ui/MainWindow.cpp:2546`), "Unsaved work from a previous session was
found. Recover it?" (`:1458`), "The export failed. Nothing was written to that
name." (`src/ui/RenderJobs.cpp:114-115`). The domain terms that remain (MIDI,
bus, insert) are the audience's own vocabulary, which the same section allows
for a specialist audience.

**B-6. "use standard controls the way they were intended" / "this order should
either be left-to-right, top-to-bottom or in a counter-clockwise circle"** (2,
*Good Software Uses Graphical Controls Properly* / *…Natural Layout to its
Controls*)

**Status: Met.** Controls are the kit's (`DawButton`, `DawSlider`,
`DawCheckBox`, `BListView`, `BStatusBar`) and layouts come from the Layout Kit,
which orders focus by construction (`docs/agent-prompts/22-layout-docking-PR.md`).
Tab order in the two widest windows is settled by the kit's construction order.
Not re-verified on screen — the `DAW_UI_SHOTS` pass is what proves it.

---

# Part C — HIG 3, Conventions of Haiku

**C-1. "If your software needs only one file … place it in the user's
config/settings folder. However, should you need more than one file, please
put them in their own folder … The folder should either follow the format
home/config/settings/your_app_name_here or
home/config/settings/your_company_name_here/your_app_name_here."** (3,
*Program Settings: Format and Location*)

**Status: Met.** The app needs three things, so it uses the folder form:
`~/config/settings/HaikuDAW/` holds `settings`
(`src/ui/ProjectDocument.cpp:67-73`), `recovery.dawproj` (`:75-82`) and
`plugins/` (`src/main.cpp:73-77`). Every path is resolved through
`find_directory`, never written out.

**C-2. "the easiest and recommended method is placing them in a BMessage and
flattening it to a BFile. By using BMessages as your container, you don't have
to concern yourself with writing and debugging other code."** (3, *Program
Settings: Format and Location*)

**Status: Deviates — Low.** Our settings file is a line-oriented text format
with its own parser: `AppSettings::Serialize`/`Deserialize`
(`src/app/AppSettings.cpp:16-100`), one `key value` line each, unknown keys
ignored so the format can grow (`src/app/AppSettings.h:1-7`).

*Why it is deliberate, and why it still counts.* `AppSettings` is in the
kit-free core so it round-trips in host tests (`tests/appsettings_tests.cpp`);
`BMessage` is a Haiku-only class, and moving the container into it would move
the whole preference layer — plus its test — out of the host suite. The HIG's
argument for `BMessage` is that you then "don't have to concern yourself with
writing and debugging other code", which is exactly the cost we did pay, once,
in 90 lines with a test. Nothing a user sees changes either way.

*Cost of changing it.* Move `Serialize`/`Deserialize` behind a
Haiku-only `SettingsFile` in `src/ui/` that reads and writes a flattened
`BMessage` with the same keys; keep the struct. One new file, ~80 lines, plus
deleting `tests/appsettings_tests.cpp`'s coverage or repointing it. Add a
one-shot migration for an existing text `settings` file, or the first run after
the change silently loses buffer size, pane sizes and the recent list. Half a
day, and it buys conformance with a "recommended", not with a rule.

*Router:* **Marc's decision** (product/architecture; worklist item 21).
If it is changed, it goes with **T6** — the Preferences window is where the
settings surface is next touched.

**C-3. "spawn another thread to do the actual processing and allow the
message-handling thread to continue to do its job."** (3, *Maintain
Responsiveness*)

**Status: Deviates — Low.** Almost everything already behaves: the audio graph
runs on its own thread, export renders on a worker with a progress bar, peaks
are built off the looper. One path does not — decoding a soundfont blocks the
Instrument window's own looper, which is why the code took the shortcut audited
at **H-1**:

> `src/ui/InstrumentWindow.cpp:153-155` — *"Decoding a big kit takes seconds
> and blocks this window's looper — show a real busy cursor so it doesn't just
> look wedged."*

The comment is honest about the cause. The HIG's prescription is the same one:
*"If you have a need for a busy cursor, you need to make your program more
responsive. Often you can use multiple threads to eliminate the need for a busy
cursor."* Fix as one item with H-1. *Router:* **T15** (worklist item 29).

**C-4. "Whenever your program needs to specify a particular location on the
system, use the find_directory() function to generate it."** (3, *Avoid
Hardcoded File Paths*)

**Status: Met.** `find_directory(B_USER_SETTINGS_DIRECTORY, …)` at
`src/main.cpp:74` and `src/ui/ProjectDocument.cpp:69`, `:79`. No literal
`/boot/home` or `config/settings` string is built by hand anywhere in `src/`.

**C-5. "applications that deal with files should provide the user with a list
of files they have opened or saved most recently."** (3, *Easy Access to
Recently Used Files*)

**Status: Met.** A File ▸ Open Recent submenu fed from `AppSettings` (last 10,
most recent first), rebuilt on load and save, and pruned when a file has
vanished (`src/ui/MainWindow.cpp:298-299`, `:2524-2536`, `RebuildRecentMenu`;
`src/app/AppSettings.h:32-38`). Record: `docs/agent-prompts/13-file-menu-PR.md`.
Two sub-clauses deviate — C-6.

**C-6. "It's recommended to show the last 10 opened or saved files in a
submenu of the Open menu item in the File menu … The number of recent items
should be limited to no more than 5 items."** (3, *Easy Access to Recently Used
Files*; 9, *File*)

**Status: Deviates — Low.** Three small things, all in the same submenu:

1. The list is a *sibling* of Open, not a submenu attached to it
   (`src/ui/MainWindow.cpp:297-299`). The chapter 3 wording is "in a submenu of
   the Open menu item"; StyledEdit, the HIG's own example, attaches it to Open.
2. `kMaxRecent = 10` (`src/app/AppSettings.h:33`) where chapter 9 says "no more
   than 5". **The HIG contradicts itself here** — chapter 3 recommends 10,
   chapter 9 caps at 5 — so this is not a defect to fix silently; pick one and
   record why.
3. The label is "Open Recent" where the HIG's examples are sentence-cased
   ("Open recent") — see F-4.

*Router:* items 1 and 3 → **T14** (worklist item 23); item 2 → **Marc's
decision** (worklist item 22, trivial).

**C-7. "Unless there is a very good reason for it, get colors for your program
with ui_color() and the constants which go with it … don't reimplement the
look of the buttons and other standard controls just to make your application
stand out from the rest."** (3, *Make Your App's Look Fit in with Others*)

**Status: Deviates — Medium — and already fixed on an unmerged branch.**
`master` calls `set_ui_color` seventeen times at startup
(`src/main.cpp:129-148`, invoked at `:151`), which does two things the HIG
argues against. It overrides every system colour instead of reading them, and —
worse than a HIG matter — Haiku *saves* those colours, so the user's Tracker and
every other application stay recoloured after the DAW quits. That is the bug
Marc decided against in `docs/UI_GUIDELINES.md` §5 and
`docs/agent-prompts/24-continue-1.0-ENTRY.md` §3(1).

`feature/theme-modes` (**T1**) removes all seventeen calls, derives the colour
tokens from `ui_color()` in System mode (the default), draws kit controls
through `be_control_look`, and keeps the dark palette as a per-process option
that cannot leak:
`docs/agent-prompts/25-theme-modes-PR.md` — *"`DawApplication::ApplyThemeColors()`
— 17 `set_ui_color` calls — is gone … What is left is per-process state the app
owns."* **Nothing to do here beyond merging T1**; recorded so that a later
audit does not re-report it against `master`.

**C-8. "Determine the size of your controls dynamically - use the
ResizeToPreferred and GetPreferredSize for system controls … If you're writing
code specifically for Haiku … use the new layout API."** (3, *Make Your App's
Look Fit in with Others*)

**Status: Met.** Every dialog and the main window are `BLayoutBuilder`
(`docs/agent-prompts/22-layout-docking-PR.md`; `src/ui/ExportProgressWindow.cpp:23-33`
is a typical one); metrics scale from the system font through `Themed()`
(`src/ui/Theme.h:25-58`), and a 150 % scale is exercised by
`SetThemeScaleOverride()` in `ui_functional_tests`. The one control that
ignores it is F-5.

**C-9. "provide 'live' updates to information in it."** (3, *Live Updates*)

**Status: Met.** Model edits invalidate and repaint through one path, the dock
and inspector follow the model, and T1 adds `B_COLORS_UPDATED` handling so a
system Appearance change re-derives the tokens live
(`docs/agent-prompts/25-theme-modes-PR.md`).

**C-10. "Merely including the library and making use of even something as
simple as the BTranslationUtils class will help to ensure that at least this
portion of your code is free of bugs."** (3, *Translators*)

**Status: Not applicable.** The app loads no external image files — every
glyph is drawn (`src/ui/widgets/DawIcons.h`) or is a kit control. There is no
translator to use.

**C-11. "By attaching attributes to audio files, you can search for them with
a query."** (3, *Squeezing the Most Out of BFS: Queries and Attributes*)

**Status: Met.** The sample browser runs a live `BQuery`
(`src/ui/SampleBrowser.cpp:108-115`) and reads BFS attributes for duration and
BPM (`:125-127`); importing a file writes the duration attribute so the query
can find and sort it (`src/ui/MainWindow.cpp`, `ImportAudioAt`). This is the
`BQuery`+attribute pattern the section describes.

**C-12. "install icons in the Deskbar's shelf only when it provides information
that is updated often or if it provides functionality which will be frequently
accessed by the user."** (3, *The System Tray*)

**Status: Not applicable.** No Deskbar shelf icon is installed, which is the
correct choice for this app by the same paragraph.

**C-13. "The Open and Save windows have already been done for you. Please use
them. If just a certain type of entry is needed … make a filter that shows only
those types of entries."** (3, *Tracker and its Uses*; repeated at 11, *Open
and Save Panels*)

**Status: Deviates — Low.** The panels are used (good) but there is not one
ref filter on any of them. Every panel is built with node flavours `0` and no
`BRefFilter*`:

- `src/ui/MainWindow.cpp:1611-1613` — Save Project
- `:1652-1654` — Open Project
- `:1678-1680` — Import Audio
- `:808-810` — Import MIDI, `:824-826` — Export MIDI
- `:2438-2440` — Locate (missing media)

So Open Project lists every file on the disk, Import Audio lists every file
including ones it will refuse, and the only filter in the tree is the
soundfont one (`src/ui/InstrumentWindow.cpp:43`, `:336`). The same section
warns not to over-filter — *"there may be files that may not have had their
MIME type identified, so do not exclude them either"* — so the filter should
accept the type plus unfiled entries, not a strict extension match.
*Router:* **T15** (worklist item 17). *Severity:* Low (usability, not
correctness).

**C-14. "Remember the location the user was viewing … Offering a possible file
name when saving a new document … The title of the panel needs to match the
task."** (11, *Open and Save Panels*)

**Status: Met.** Directory memory in `AppSettings.lastDir` and in the relink
walk, which reopens at the folder the file used to live in
(`src/ui/MainWindow.cpp:2451-2458`); a prefilled name for every save panel
(`ShowPanel`, `:158-171`, called at `:763`, `:774`, `:827`, `:1614`);
titles name the action — "Save Project", "Export Mix", "Import Audio",
"Locate take-2.wav" — never the stock "<app>: Save" (`:767-774`, `:2461`).
This is stronger than the HIG asks and is enforced by
`docs/UI_GUIDELINES.md` §3.

---

# Part D — HIG 4, Getting input from the user

**D-1. "Single-click … Double-click on primary mouse button … Single click
with secondary mouse button … Skills the user might have … Don't Use These:
Click-and-a-half … More than 3 consecutive clicks … Drag with tertiary mouse
button …"** (4, *Mouse Vocabulary*)

**Status: Met.** The timeline uses single click (select, seek), double click
(open the editor), secondary click (context menu — the pattern the same section
names) and primary drag (move, trim, fade, gain with Ctrl,
`src/ui/TimelineView.cpp:801`). No click-and-a-half, no quadruple click, no
tertiary-button binding, no modifier + secondary click.

**D-2. "Scroll Wheel + B_COMMAND_KEY: Zoom in/out; + B_OPTION_KEY: Scroll by
one full page; + B_CONTROL_KEY: Increase/decrease font size"** (4, *Mouse
Vocabulary*)

**Status: Not applicable today — but a plan conflict to fix before it becomes
one.** The only wheel binding on `master` is plain vertical scroll
(`src/ui/TimelineView.cpp:175-181`); no modifier+wheel combination is taken,
so nothing is violated. However `docs/PLAN_1.0.md` M2.2 specifies *"the zoom
anchors on the mouse position (Ctrl+wheel; keys −/+ anchor on the playhead)"* —
Ctrl+wheel, where the HIG gives zoom to **Command**+wheel and Ctrl+wheel to
font size. *Router:* **T4** — fix the plan line when M2.2 is implemented
(worklist item 30, trivial, plan text only).

**D-3. "Standard System Accelerators: B_COMMAND_KEY + N … + O … + S … + Shift
+ S … + W … + Q … + C … + X … + V … + A … + Z … + B_SHIFT_KEY + Z"** (4,
*Keyboard Accelerators*)

**Status: Met for every accelerator the app implements**, all from one block
in `src/ui/MainWindow.cpp`:

| Key | Item | Line | Status |
|---|---|---|---|
| Cmd+N | File ▸ New | `:296` | Met |
| Cmd+O | File ▸ Open… | `:297` | Met |
| Cmd+S | File ▸ Save | `:302` | Met |
| Cmd+Shift+S | File ▸ Save As… | `:303-304` | Met |
| Cmd+W | File ▸ Close | `:313` | Met |
| Cmd+Q | File ▸ Quit | `:314` | Met |
| Cmd+Z | Edit ▸ Undo | `:317` | Met |
| Cmd+Shift+Z | Edit ▸ Redo | `:318` | Met |
| Cmd+V | Edit ▸ Paste | `:320` | Met |
| Cmd+C / Cmd+X / Cmd+A | Copy / Cut / Select All | — | **missing on `master`** |

The three missing ones are the menu-content gap at **I-5**, not a wrong
binding: nothing else has taken those keys. They are implemented on
`feature/arrange-depth` (M2.4) with the correct accelerators.
Cmd+P (Print) does not apply — the app does not print — and the Cmd+B/U/I
group is not used for text styling (Cmd+I is taken for the Inspector, which
D-4 discusses).

**D-4. "Choose key combinations which are easily associated with the functions
that go with them … Do not use one of the system combinations for a task
different from the list above. Doing so will only confuse and frustrate
users."** (4, *Keyboard Accelerators*)

**Status: Deviates — Medium.** One clear violation and one debatable:

- **Cmd+F is View ▸ Zoom to Fit** (`src/ui/MainWindow.cpp:351`). The standard
  list assigns Cmd+F to *"Show find window"*. This is precisely the reuse the
  paragraph forbids, and Cmd+F is one of the most reflexive shortcuts a Haiku
  user has. A DAW has no find, so nothing else wants the key — which is exactly
  why taking it for zoom is the kind of thing this paragraph is about.
  *Router:* **T14** (worklist item 6); if Marc prefers to keep a one-key zoom,
  it is a **Marc's decision** — but then it should not be Cmd+F.
- **Cmd+I is View ▸ Inspector** (`:345`). Cmd+I is on the standard list, but
  annotated *"(Word processor) Italicized font"*, so for a non-word-processor
  it is arguably free. Flagged as a judgement call, not a defect.
  *Router:* **Marc's decision** — no change recommended.
- Cmd+J for the dock (`:348`) is not on the standard list and collides with
  nothing. Fine.

**D-5. "B_COMMAND_KEY + ,: Settings window"** (4, *Keyboard Accelerators*)

**Status: Deviates — Low.** There is no Settings item and no Cmd+, binding,
because there is no preferences window yet: buffer size, count-in, monitor
input and the export choices live as menu items and dialog state
(`src/ui/MainWindow.cpp:362-395`). This is not an oversight; it is the state
M3.5 exists to change (see K-5). *Router:* **T6** — the Preferences window
carries the Cmd+, accelerator and a Program ▸ Settings… item (worklist item 9).
Already recorded in `docs/PLAN_1.0.md` M3.5.

**D-6. "make controls easy to click on … do keep in mind that teeny controls
slow users down."** (4, *Design to Prevent Fitts*)

**Status: Met.** Hit targets are font-scaled through `Themed()` and the kit
widgets enforce a minimum (`src/ui/MainWindow.cpp:145-152`, `HeaderButton`,
floors the height at `Themed(18)` and the width at `Themed(22)`); the transport
bar and timeline rects are all `Themed()` metrics
(`docs/agent-prompts/20-theme-PR.md`). The MIDI tool palette is icon-only and
carries hover tooltips to make up for the smaller targets
(`docs/agent-prompts/README.md`, M1.6 icon note).

---

# Part E — HIG 5, Drag & drop and the clipboard

**E-1. "When implementing dragging, be sure that your program provides
feedback about what is being dragged. This is often best done by using an image
which closely represents what is being dragged."** (5, *Drag and Drop*)

**Status: Met.** The sample browser drags with the item's own frame as the
drag image (`src/ui/SampleBrowser.cpp:40` — `DragMessage(&drag,
ItemFrame(index), this)`), so the drag looks like the row it came from.

**E-2. "A feature which is very helpful to a user is drop feedback. Any time
the user is dragging something and the cursor passes over your program's
window, it can react in a way to show the user that your program will accept
the dragged object."** (5, *Drag and Drop*)

**Status: Deviates — Low.** The drop works but nothing happens *before* the
button is released. `TimelineView` handles `B_SIMPLE_DATA` only after the drop
(`src/ui/TimelineView.cpp:202-243`) and never overrides `DragEnter`/`DragMoved`/
`DragLeave`; there is no highlight on the target lane, no line where the clip
would land, and a dropped file that is not audio or MIDI fails only as a line
on stderr (`src/ui/TimelineView.cpp:238-241`). The HIG lists exactly these as the forms feedback takes
(a line between items, a shaded folder, a highlighted border). *Router:*
**T4** (M2.1 *Pointer feedback* is the item that already owns hover states —
worklist item 18). *Severity:* Low.

**E-3. "Another way of getting data from one program into another is via the
system clipboard … If the amount of data you wish to pass to another program is
too large to place on the clipboard without straining the system, dump the data
to a temporary file and place an entry_ref which points to it on the clipboard
instead. Audio and video files commonly suffer from this problem."** (5, *The
Clipboard*)

**Status: Deviates — Low — product call.** `master` has Edit ▸ Paste but no
Copy or Cut, and grep finds no `BClipboard` anywhere in `src/`: the clip
clipboard is a private vector in `TimelineView`
(`feature/arrange-depth` adds `fClipboard` and Cut/Copy/Select All, still
internal). So a region copy cannot leave the app, and the HIG's own example of
the case where the clipboard matters — audio that is too large to copy — is
this app's normal payload. In practice a DAW's cross-application clipboard is a
nicety (no other app on the machine consumes an audio region), which is why
this is a product call rather than a defect. *Router:* **Marc's decision**
(worklist item 24, small/medium). If yes, the HIG already says the shape:
`entry_ref`s under the `refs` field.

**E-4. "It is appropriate for a program to be a replicant if it is a
lightweight program … Replicants take their cue … 32 pixels square is a good
minimum size …"** (5, *Replicants*)

**Status: Not applicable.** No replicant support, which the same section frames
as an optional extra rather than an expectation.

---

# Part F — HIG 6, Use of text in the GUI

**F-1. "the error message given to the user should … Offer suggestions to help
the user fix the problem, if possible."** (6, *Error Messages*)

**Status: Deviates — Low.** Most messages do this well — "Locate them, or skip
and leave those clips silent." (`src/ui/MainWindow.cpp:2425`). One does not:

```
src/ui/RenderJobs.cpp:114-116
BAlert("Export", "The export failed. Nothing was written to that name.",
       "OK", …)
```

It says what happened and that nothing was written (useful), but not *why* —
the reason (disk full, bad path, no space, a failed create) goes to stderr two
lines above and is thrown away for anyone not running from a Terminal, and
there is no suggestion. The HIG's own worked examples always pair the two
("MyApp can't copy your file to the disk because there isn't enough space for
it. If you would like to copy it, move or delete files on the destination disk
and try again."). *Router:* **T15** (worklist item 20, trivial) — pass the
failure code into the alert and give one line of advice.

**F-2. "An ellipsis is a series of 3 dots (…) used to tell the user that a
control … will require further input … Please be sure to use the
B_UTF8_ELLIPSIS character instead of 3 periods."** (6, *…Ellipses…*)

**Status: Met.** Every menu item that opens a window or a panel carries
`B_UTF8_ELLIPSIS` — Open… (`src/ui/MainWindow.cpp:297`), Save As… (`:303`),
the five Import/Export items (`:305-309`), About (`:328`), Master Effects…
(`:336`), Keyboard Shortcuts… (`:355`), Sample Browser… (`:357`) — and the
Locate… alert button (`:2428`). The rule is applied in the *negative* direction
too, which is the part most apps miss: Save has no ellipsis because it usually
writes straight to the file (`:301-302`), and New has none because it shows no
window (`:296`, `NewProject()` at `src/ui/MainWindow.cpp:2483`). The audit found no `"..."` in any
menu or button label.

**F-3. "Menus and button labels should never be abbreviated or contain an
acronym."** (6, *Abbreviations, Acronyms, and Contractions*)

**Status: Met.** No menu label is abbreviated. "WAV" and "MIDI" appear in
Import/Export items; both are the standard terms for the audience this app is
for, which the same section allows, and the HIG's own example of permitted
specialist vocabulary (CMYK in a colour picker) is the identical situation.

**F-4. "Use sentence capitalization in all places, that is, follow the normal
grammar rules of the language. For English, this means only to capitalize
specific names like 'Tracker' or 'Deskbar'."** (6, *Capitalization and
Spelling*)

**Status: Deviates — Low.** The menu labels are Title Case, and the HIG's own
chapter 9 uses sentence case for the same items:

| Ours | `src/ui/MainWindow.cpp` | HIG's form |
|---|---|---|
| Open Recent | `:298` | Open recent |
| Save As… | `:303` | Save as… |
| Import Audio… / Import MIDI… | `:305-306` | Import from… |
| Export WAV… / Export Stems… / Export MIDI… | `:307-309` | Export to… |
| Master Effects… | `:336` | (Master effects) |
| Monitor: Dim / Monitor: Mono | `:339`, `:341` | (no capitals after colon) |
| Zoom to Fit | `:351` | Zoom to fit |
| Follow Playhead | `:352` | Follow playhead |
| Keyboard Shortcuts… | `:355` | Keyboard shortcuts |
| Select All (on `feature/arrange-depth`) | `:350` | Select all |

`feature/arrange-depth` already uses the sentence-cased "Select All" in the
HIG's exact wording, so the two forms are mixing. Purely mechanical, one
pass over the `BMenuItem` constructors plus the shortcuts table
(`src/model/KeyMap.h` on `feature/keymap-meters`) and the USER_GUIDE. *Router:*
**T14** (worklist item 4). *Severity:* Low.

**F-5. "Stick to the plain, bold, and fixed system fonts … Control labels
should always be in the system plain font and size specified by the system …
do not depend on the system font being set to a particular font size."** (6,
*Use of Fonts*)

**Status: Deviates — Low.** Fonts are right (`be_plain_font`/`be_bold_font`
only, sizes derived in `src/ui/Theme.h:22-64`), and the sizes the app does vary
are headings and readouts — the LCD at `Themed(15)`
(`src/ui/MainWindow.cpp:404-405`), the loudness readout at `Themed(10.5)`
(`:461-463`), the dock title at `Themed(11.5)` (`:499-500`) — all scaled from
the system size, which is what the section asks. **One control does not scale:**

```
src/ui/InspectorView.cpp:181
BFont bold(be_bold_font); bold.SetSize(13); SetFont(&bold);
```

13 is a literal: it ignores the user's font size and stays 13 while every
other metric grows, so at a 150 % scale (or with a larger system font) the
track name is the one label that does not follow — which is exactly what
`SetThemeScaleOverride()`/`TestThemeScale` exists to catch. *Router:* **T15**
(worklist item 12, trivial) — `Themed(13.0f)` or, better, `ThemeFontBold()`.
*Severity:* Low.

**F-6. "Provide GNU-style (double dash + long name) switches for all options.
The most commonly-used options should also have a short (single-dash + single
letter) counterpart."** (6, *Special Case: The Command Line*)

**Status: Met.** The app has exactly one option and it follows the rule:
`--version` (`src/main.cpp:267-272`), printed before the `BApplication` is
constructed so it answers from a Terminal on a machine with no app_server and
a running instance cannot swallow it. Everything else on the command line is a
project path, which is the file-oriented interface the same section describes.
The app requires no parameters (a bare launch starts a demo session), so the
"show help when parameters are missing" clause does not apply.

**F-7. "The switches --help and -h are reserved for showing help
information."** (6, *Special Case: The Command Line*)

**Status: Deviates — Low.** Neither switch exists. Nothing else has taken
them, so the reservation is not violated — but it is also not honoured, and
the section is explicit that *all* new command-line programs should provide
help. A `--help`/`-h` that prints the usage line already in `src/main.cpp:15`
("`daw [--version] [project.dawproj ...]`") plus the accelerator list from the
one shortcuts table closes it. *Router:* **T14** (worklist item 3, small),
folded with the Help-menu work, since both read the same table. *Severity:*
Low.

---

# Part G — HIG 7, Branding

**G-1. "Take some time to create or find a good-looking icon. Whatever you do,
don't just slap together a shabby-looking icon. It would be better not provide
an icon at all and rely on the system to show the default application icon
than to have one which reflects poorly on your program's reputation."** (7,
*Program Icons*)

**Status: Deviates — Low — deliberately deferred.** Judged by the HIG's own
sentence, `master` does the *acceptable* thing: `haiku-daw.rdef.in` carries
`app_signature`, `app_flags` and `file_types` but no `vector_icon` block, and
says so —

```
haiku-daw.rdef.in:1-6
 * No icon yet: a vector_icon block belongs here once there is one, and until
 * then the app shows the generic one.
```

so Tracker and Deskbar show the default application icon rather than a bad one.
It is still a gap against the publication checklist (**N-2**), and it is the
largest remaining visible piece of Haiku-ness the app lacks. The rules the icon
must follow are collected at **O-1**/**O-2**. *Router:* **T12** (already
planned and Marc-gated; worklist item 26).

**G-2. "make an About window … It should contain the title of your program, the
version, you and any other authors or the name of your company, and copyright
information … The window should not have a tab and should either have a button
marked 'Close' or simply disappear when clicked. It should also respond to the
Command + W keyboard shortcut."** (7, *About Windows*)

**Status: Deviates — Medium.** The About box is a `BAlert` built in place:

```
src/ui/MainWindow.cpp:1425-1436
case MSG_ABOUT: {
    …
    std::snprintf(text, sizeof(text),
                  "Haiku DAW %s\n\nA native digital audio workstation "
                  "for Haiku:\nmultitrack audio and MIDI, mixing, "
                  "automation,\nLV2 plugins, and offline export.",
                  DAW_VERSION_STRING);
    BAlert* a = new BAlert("About Haiku DAW", text, "OK", nullptr,
                           nullptr, B_WIDTH_AS_USUAL, B_INFO_ALERT);
    a->Go(nullptr);
```

Going through the requirement clause by clause:

- **title of the program** — met ("Haiku DAW").
- **the version** — met, from the one generated header.
- **you and any other authors or the name of your company** — **missing.** No
  author or company line at all.
- **copyright information** — **missing.**
- **should not have a tab** — met (a `BAlert` is a bordered window with no
  tab).
- **a button marked 'Close' or disappear when clicked** — **not met**: the
  button says "OK".
- **respond to Command + W** — **not met.** The alert is posted asynchronously
  with a null owner (`a->Go(nullptr)`) and nothing anywhere handles Cmd+W for
  it; `Cmd+W` is wired only to File ▸ Close, which posts `MSG_CLOSE` to the main
  window (`:313`, `:1630-1632`).
- (Also worth noting the HIG's escape hatch — *"If nothing else, write a few
  lines of code to show a BAboutWindow with this information"* — which the app
  does not use; grep finds no `BAboutWindow` in `src/`.)
- The one advisory it already honours: *"Do not include information about the
  computer itself."* There is none.

*Router:* **T14** (worklist item 2, small). The MIT licence line ("Distributed
under the terms of the MIT License", with the full text left in `LICENSE`) is
what the same paragraph permits alongside.

**G-3. "Graphics need not be limited to just the About window and the program
icon … Follow the Dos and Don'ts for toolbars mentioned in Chapter 12"** (7,
*Graphics and Other Stuff*)

**Status: Met.** The only graphics are the drawn toolbar and transport glyphs
(`src/ui/widgets/DawIcons.h`), each with a hover tooltip, and the keyboard
drawn into the piano roll. Met against L-10.

---

# Part H — HIG 8, Cursors

**H-1. "NOTE: There is not nor will there ever be a busy cursor in BeOS-based
operating systems. This is a deliberate design decision. If you have a need
for a busy cursor, you need to make your program more responsive. Often you
can use multiple threads to eliminate the need for a busy cursor."** (8,
*Making Your Own Cursors*)

**Status: Deviates — Medium.** The app pops a busy cursor, app-wide:

```
src/ui/InstrumentWindow.cpp:153-160
// Decoding a big kit takes seconds and blocks this window's looper —
// show a real busy cursor so it doesn't just look wedged. (The audio
// thread is unaffected; only this editor stalls.)
BCursor busy(B_CURSOR_ID_PROGRESS);
if (be_app) be_app->SetCursor(&busy);
std::string err;
inst = cache.Load(fDesc.path, fDesc.sf2Preset, &err);
if (be_app) be_app->SetCursor(B_CURSOR_SYSTEM_DEFAULT);
```

This is the rule the HIG states most absolutely in the whole document — "not
nor will there ever be" — and the code is also honest about the cause in its own
comment: the decode blocks the window's looper. The remedy the paragraph names
is the remedy that applies: move `SoundfontCache::Load` off the window thread
(the audio thread is already unaffected, and `Load` is a cache operation, not a
DSP one) and report progress the way export does, through the pulse. Fixing the
threading removes the need for the cursor; with the decode off-thread, delete
both `SetCursor` calls. *Router:* **T15** (worklist item 29, small/medium),
together with **C-3**, which is the same code and the same root cause.
*Note for whoever takes it:* `be_app->SetCursor` is also app-wide, so even
while it is up it changes the pointer over every window.

**H-2. "You can very easily make your own cursors for your own purposes, but
do it only if a different cursor will dramatically improve how well the user
can work with your program … Be sure that the hot spot … is very obvious. Use
the full dimensions available."** (8, *Making Your Own Cursors*)

**Status: Met.** No custom cursor is defined anywhere in `src/` — only the
stock `B_CURSOR_SYSTEM_DEFAULT` and the one at H-1. Nothing to check.

**H-3. "While Haiku does not provide explicit support for them, you can
animate cursors to provide a little more graphical appeal."** (8, *Animating
Cursors*)

**Status: Not applicable.** No cursor animation.

---

# Part I — HIG 9, Menus, menu bars and menu fields

**I-1. "Items should be organized and grouped by function and/or attribute.
Use a separator item between each item group."** (9, *Naming and Organization*)

**Status: Met.** Separators are present at every group boundary in File
(`src/ui/MainWindow.cpp:310`, before Close/Quit), Edit (`:319`, between
Undo/Redo and the clipboard group — the HIG's own worked example), View
(`:338`, `:343`, `:350`), and Audio (`:377`). Menus are named for what they
hold: File for documents, Edit for edits, Track for tracks, View for what is on
screen, Audio for engine settings.

**I-2. "The preferred method is to place a checkmark beside the item indicating
the positive state … When there are more than two states, all possible states
need to be listed."** (9, *Marking and Toggling Items*)

**Status: Met.** Every stateful item is a checkmark rather than a swapped
label: Metronome, Monitor: Dim, Monitor: Mono, Inspector, Follow Playhead
(`src/ui/MainWindow.cpp:334-354`), and the two multi-state menus list every
value and are radio-mode (`fBufMenu->SetRadioMode(true)` `:364`,
`fCountInMenu->SetRadioMode(true)` `:380`, marked by `MarkRadio` at `:2789`).

**I-3. "Program: Items related to operating on the program itself. About <app
name here>… / Settings… / Quit (Command + Q) … This should be the bottom item
in the menu and a separator should go above it."** (9, *Common Menus and their
Contents*)

**Status: Deviates — Medium.** There is no Program menu. `menuBar` is built as
File, Edit, Track, Help, View, Audio (`src/ui/MainWindow.cpp:295-395`), so:

- **About** is in **Help** (`:327-329`), not in a Program menu.
- **Settings…** does not exist anywhere (see D-5 / K-5).
- **Quit** is the last item of **File**, with a separator above it
  (`:313-314`). The separator part is right; the menu is wrong.

*Router:* **T14** (worklist item 1). The Settings… item itself belongs to
**T6**, because the window it must open is M3.5's.

**I-4. "File: This contains items related to documents handled by your
program. New (Command + N) … Open… (Command + O) … Open recent … Close
(Command + W) … Save (Command + S) … Save as… (Command + Shift + S) …
Import from… … Export to…"** (9, *Common Menus and their Contents*)

**Status: Met.** Every item that applies is present and correctly keyed:
New (`:296`, no ellipsis, correct because it shows no window), Open… `:297`,
Open Recent `:298-299`, Save `:302` (no ellipsis, correct — it writes straight
to the path once there is one), Save As… `:303-304`, the Import/Export group
`:305-309`, Close `:313`, Quit `:314`. Save-all and Revert do not apply (one
document); Page setup and Print do not apply (no printing); both are correctly
absent rather than present-and-dead. Old-project *Revert* is not something a
DAW offers because the undo stack covers it. The ordering differs from the
HIG's listing and the grouping is coherent (document group, then
save/import/export, then Close/Quit), which is what I-1 asks for.

**I-5. "Edit: Undo (Command + Z) / Redo (Command + Shift + Z) / Cut (Command +
X) / Copy (Command + C) / Paste (Command + V) / Select all (Command + A)"**
(9, *Common Menus and their Contents*)

**Status: Deviates — Medium — and already fixed on an unmerged branch.** On
`master` the Edit menu holds only Undo, Redo, a separator and Paste
(`src/ui/MainWindow.cpp:317-320`). Cut, Copy and Select All are missing, so a
user looking for the standard Edit menu finds three of six items, and the
timeline's own Ctrl-D duplicate has no menu home.

`feature/arrange-depth` (M2.4) builds the full menu —
Cut Cmd+X, Copy Cmd+C, Paste Cmd+V, Delete, Select All Cmd+A, Duplicate Cmd+D,
each enabled and disabled against the selection — and is the branch that
carries the M2.4 work in `docs/PLAN_1.0.md`. **Nothing to do beyond merging
it.** *Router:* **T4**.

**I-6. "When possible, this item should be dynamic and also include the name of
the task that it would undo, such as 'Undo Cut' or 'Undo Typing.'"** (9,
*Edit*)

**Status: Deviates — Low — and already fixed on an unmerged branch.** The model
already exposes the name (`src/model/Command.h:112`, `UndoName()`), but on
`master` the menu items are static labels ("Undo", "Redo",
`src/ui/MainWindow.cpp:317-318`) and grep finds no `SetLabel` in
`MainWindow.cpp`. `feature/arrange-depth` wires it up
(`fUndoItem->SetLabel(...)` from `fStack->UndoName()`), so the HIG's "Undo
Paste"/"Undo Move Clip" behaviour arrives with M2.4. *Router:* **T4** — merge,
no new work.

**I-7. "Search: Tasks in this menu include finding and replacing data and other
navigation commands. Find… (Command + F) / Find again (Command + G)"** (9,
*Common Menus and their Contents*)

**Status: Not applicable.** There is no document search in the app. Note the
knock-on: because there is no Find, Cmd+F is free — which is what made D-4's
reuse easy to slip in, and does not make it correct.

**I-8. "Help: Different ways that the user can learn more about your program
and get help when needed. Open manual… / Go to (MyApp or MyCompany)'s
website"** (9, *Common Menus and their Contents*)

**Status: Deviates — Medium.** The Help menu has exactly one item:

```
src/ui/MainWindow.cpp:327-330
BMenu* helpMenu = new BMenu("Help");
helpMenu->AddItem(new BMenuItem("About Haiku DAW" B_UTF8_ELLIPSIS,
                                new BMessage(MSG_ABOUT)));
```

— which is not help at all (and belongs in the Program menu, I-3). There is no
**Open manual…**, and this is the sharper half of the finding: the repository
**ships a manual** (`docs/USER_GUIDE.md`, 111 lines, plus
`docs/RELEASE_CHECKLIST.md` and the README) and **no code path opens it** —
`grep -rn "USER_GUIDE" src/` returns nothing, and the only hits for "manual"
in `src/` are unrelated comments in the DSP and UI code. The in-app key list is a `BAlert` (View ▸ Keyboard Shortcuts…,
`:1517-1529`), which is the one form the HIG rules out for a manual: *"The
manual should never be shown in a BAlert, prefer opening an HTML or read-only
text file (using the preferred application set by the user), or showing a
non-editable text view from which the text can be copied."* There is also no
website item (the README links <https://www.haiku-os.org/> and the project has
a GitHub page).

*Router:* **T14** (worklist item 3, small/medium) — Help ▸ Open manual…
opening `USER_GUIDE.md` with the user's preferred application (the HIG's first
preference), Help ▸ Haiku DAW website, and a real shortcuts window.
`feature/keymap-meters` already replaces the shortcuts `BAlert` with
`src/ui/ShortcutsWindow.cpp` (a scrolled read-only `BTextView`) built from one
kit-free table (`src/model/KeyMap.h`), which is the pattern the manual item
should reuse; that branch is M3.2 and unmerged.

**I-9. "When organizing the menus in a menu bar, try to have a logical
progression from one menu to another. A financial program might have these
menus: Program, File, Account, Transaction, and Help."** (9, *Naming and
Organization*)

**Status: Deviates — Low.** Our bar order is File, Edit, Track, **Help**,
View, Audio (`src/ui/MainWindow.cpp:295-395`). Help sits in the middle; the
HIG's own example puts Help last and the program-related menu first, and every
Haiku app the user has met keeps Help hard right. With I-3's Program menu added
the natural order is Program, File, Edit, Track, View, Audio, Help. *Router:*
**T14** (worklist item 5) — one-line reordering, but it should land with I-3 so
the bar is rebuilt once.

---

# Part J — HIG 10, Windows

**J-1. "Haiku allows you to specify the look and feel when creating a window.
Setting these properly will help the user immediately know what kind of window
they are dealing with … Document Windows containing a user document … Floating
Tool and utility windows"** (10, *Styles and Purpose*)

**Status: Met.** Every window is constructed with a type that matches its job:

- main window — `B_TITLED_WINDOW` (`src/ui/MainWindow.cpp:280-281`), zoomable,
  `B_QUIT_ON_WINDOW_CLOSE`;
- utility windows — `B_TITLED_WINDOW` + `B_NOT_ZOOMABLE`, and the modal-ish
  editors additionally `B_FLOATING_APP_WINDOW_FEEL`
  (`src/ui/MixerWindow.cpp:458-459`, `src/ui/PluginBrowser.cpp:54-55`,
  `src/ui/SampleBrowser.cpp:54-55`, `src/ui/PianoRoll.cpp:1073-1074`,
  `src/ui/EffectsWindow.cpp:1207-1208`, `src/ui/QuantizeWindow.cpp:47-48`,
  `src/ui/ExportWindow.cpp:69-70`, `src/ui/SendsWindow.cpp:34-35`,
  `src/ui/InstrumentWindow.cpp:74-75`, `src/ui/RenameWindow.cpp:16-18`,
  `src/ui/Lv2UiWindow.cpp:488-489`) — the "Floating:App" row of the HIG's feel
  table, which is M1.4's fix for exactly this
  (`docs/agent-prompts/22-layout-docking-PR.md`).

One window refuses to close, which the HIG allows *"unless there is a very
good reason"* and this one has: the export progress window carries
`B_NOT_CLOSABLE` (`src/ui/ExportProgressWindow.cpp:16`) but also a real
Cancel button wired to the render job (`:29-31`), so the close path is not
removed, it is replaced by the one that can actually stop the work.

**J-2. "The main window of your program should include your program's name …
the names of both the item and the window should be the same … Document
windows should include the document's name. The first new document in the
application should use the name 'Untitled' … A titled window should never have
an empty title bar."** (10, *Naming, Placement, Size*)

**Status: Met.** `ProjectDocument::Title()` (`src/ui/ProjectDocument.cpp:28`)
returns `"[*]name — Haiku DAW"`, `DisplayName()` (`:15`) returns "Untitled" for
a project with no path, and the dirty
marker is a leading `*` (`src/ui/ProjectDocument.cpp`; `UpdateTitle()` at
`src/ui/MainWindow.cpp:2382-2386`, called on every path that changes the
document). Menu-opened windows match their item: View ▸ Mixer → "Mixer"
(`:458`), View ▸ Sample Browser… → "Sample Browser" (`:54`). One exception,
folded into J-3's router: View ▸ **Master Effects…** opens a window titled
after the focused plugin, not "Master Effects"
(`src/ui/EffectsWindow.cpp:1213`, `SetTitle(EffectDisplayName(...))`);
`feature/dock-browsers` gives it `WindowTitleFor(chain, focusSlot, trackName)`
so the title names the track as well. No window in the tree is created with an
empty title.

**J-3. "An application window should have an initial size which is the minimum
needed to see all controls in it without overcrowding … This initial size
should also be the minimum size for the window which is passed to
SetSizeLimits."** (10, *Naming, Placement, Size*)

**Status: Deviates — Low.** The two numbers disagree. The window is created at
1000×560 (`src/main.cpp:170`) and then allowed to shrink to 620×300
(`src/ui/MainWindow.cpp:555`, `SetSizeLimits(620.0f, 32767.0f, 300.0f,
32767.0f)`). A user (or the restored frame from a previous session,
`:2863-2867`) can therefore reach a size where the transport bar and five
columns cannot all be drawn without crowding — the failure mode
`docs/UI_GUIDELINES.md` §3 exists to catch. HIG:
*"Do not arbitrarily restrict resizing unless it does not make sense to allow
resizing in a particular direction"*, so the fix is to raise the minimum to
what the window actually needs, not to raise it arbitrarily.
*Router:* **T15** (worklist item 15, small) — measure the real minimum during
the screenshot pass, set `SetSizeLimits` to it, and make the initial frame the
same number. *Severity:* Low.

**J-4. "Program windows should initially either show themselves in the center
of the screen or just a little above it (use BWindow::CenterOnScreen)."** (10,
*Naming, Placement, Size*)

**Status: Deviates — Medium.** `grep -rn "CenterOnScreen" src/` returns
nothing. The first window is placed by a hard-coded rectangle,
`src/main.cpp:170` — `BRect frame(80, 80, 80 + 1000, 80 + 560)` — which is
neither centred nor screen-aware: at 1000×560 plus the 80 px offset it needs
roughly 1080×640 of screen, so on anything smaller than the usual wide desktop
it starts partially off the bottom-right. The restored frame has the same
problem from the other end: `LoadSettings` accepts any saved frame whose sides
are merely non-degenerate (`s.winR - s.winL > 320 && s.winB - s.winT > 240`,
`:2863-2867`) with no `BScreen` clamp, so a frame saved on a larger monitor
comes back off-screen — the "no window off the bottom of the screen" rule in
`docs/UI_GUIDELINES.md` §3. *Router:* **T15** (worklist item 14, small) —
`CenterOnScreen()` when there is no saved frame, and clamp a restored frame
into `BScreen::Frame()` when there is. The utility windows are already sized
from their content and clamped (`FittedWindow` idiom,
`docs/UI_GUIDELINES.md` §4). *Severity:* Medium.

**J-5. "If a window allows resizing, its size should generally be saved when
closed or the program quits and restored to that size when shown again."**
(10, *Naming, Placement, Size*)

**Status: Met.** The main window's frame round-trips through `AppSettings`
(`src/app/AppSettings.h:20`, written in `SaveSettings()` `src/ui/MainWindow.cpp:2886-2887`,
restored at `:2863-2867`), along with the inspector width, the dock height and
their visibility (`:2888-2896`). `SaveSettings()` runs from `QuitRequested()`
(`:3133`). The only defect in that path is the missing screen clamp — J-4.

**J-6. "Utility windows, for example, are not intended to be the main focus of
the program, so they should not allow zooming even though they should allow
resizing except where inappropriate. Document windows should expand to fill
the most sensible space."** (10, *Zooming*)

**Status: Met.** Every utility window sets `B_NOT_ZOOMABLE` (the list at J-1),
and the main window deliberately does not, because it is the window that should
fill the screen. The dialogs that fix their size (Quantize, Rename, Export)
are the ones where resizing is "inappropriate" by the HIG's own wording; the
larger editors (Mixer, Piano Roll, Sample Browser, Effects) resize.

**J-7. "Generally, a document should allow minimizing … Utility windows and
program windows which are not the main window should normally not permit
minimizing and just allow closing. The main window should allow minimizing."**
(10, *Minimizing*)

**Status: Deviates — Low.** The main window allows minimising, correctly. So
does every utility window: `grep -rn "B_NOT_MINIMIZABLE" src/` returns
nothing, and no other flag excludes it, so the Mixer, Effects, Sample Browser,
Plugin Browser, Piano Roll, Instrument, Sends, Export, Export-progress,
Quantize and Rename windows can all be minimised to the Deskbar. The HIG's
reason is that a utility window has no document to park and minimising it only
adds Deskbar clutter. *Router:* **T15** (worklist item 16, small) — add
`B_NOT_MINIMIZABLE` to the utility windows in the J-1 list, in one pass, and
look at the result in the screenshot pass (a `B_FLOATING_APP_WINDOW_FEEL`
window is hidden when the app loses focus anyway, so the change is mostly to
the title-bar buttons). *Severity:* Low.

**J-8. "As a rule, this behavior should not be used [B_ACCEPTS_FIRST_CLICK],
but there are times when it is quite useful, such as for system monitor
programs, the Deskbar, and so forth."** (10, *B_ACCEPTS_FIRST_CLICK*)

**Status: Met.** The flag appears nowhere in `src/`, which is the "as a rule"
the paragraph asks for.

**J-9. "By default, windows in BeOS operating systems are modeless … Modality …
The need is far less often than what many would believe."** (10, *Modality*)

**Status: Met.** No window takes a `B_MODAL_*_WINDOW_FEEL`. The only modal
things are `BAlert`s, which is what they are for, and they are used sparingly
(the audit found eight call sites, all of them an error, a question the app
genuinely cannot answer alone, or the About box).

---

# Part K — HIG 11, Special purpose windows

**K-1. "On a notification, such as an error message, use 'OK' for the label of
the button."** (11, *Alert Windows*)

**Status: Met.** Every notification alert uses OK and nothing else: export
failure `src/ui/RenderJobs.cpp:114-116`, soundfont report
`src/ui/MainWindow.cpp:2319`, the generic `ReportError` `:2393`. (The About box
is the one alert that should say something else — that is G-2, a different
requirement.)

**K-2. "Avoid Yes / No button labels. It is much better to use the name of the
action in the label, such as Save Changes / Discard Changes."** (11, *Alert
Windows*)

**Status: Met — and it is one of the app's better moments.** Not one alert in
the tree has a Yes or No. The four question alerts name their outcomes:
Discard / Recover (`src/ui/MainWindow.cpp:1457-1459`), Skip / Locate…
(`:2426-2428`), Cancel / Discard / Save (`:2545-2547`). This is the wording
the HIG holds up as the example, used consistently.

**K-3. "It is *never* acceptable to ask the user 'Are you sure you want to
quit?' … If your program is asked to quit, handle unsaved documents and
quit."** (11, *Alert Windows*)

**Status: Met.** `QuitRequested()` asks `ConfirmDiscardChanges()` and stops
only when the answer is genuinely undecided:

```
src/ui/MainWindow.cpp:3126-3139
bool MainWindow::QuitRequested() {
    // Both Cmd-Q and the window's close button arrive here. Ask before stopping
    // anything, so a Cancel leaves the session exactly as it was.
    if (!ConfirmDiscardChanges())
        return false;               // stay: the user has not decided yet
```

and `ConfirmDiscardChanges()` returns true immediately for a clean project
(`:2544`). A dirty project gets the Save / Discard / Cancel prompt the HIG
sanctions, not a "are you sure" nag. Record:
`docs/agent-prompts/10-unsaved-changes-PR.md`.

**K-4. "When asking a question, make the least destructive of the most common
choices the default."** (11, *Alert Windows*)

**Status: Deviates — Low.** The Unsaved Changes prompt is fine (its first
button is Cancel and Escape is bound to it, `src/ui/MainWindow.cpp:2545-2549`).
The crash-recovery prompt is not:

```
src/ui/MainWindow.cpp:1457-1459
BAlert* a = new BAlert("Recover",
    "Unsaved work from a previous session was found. Recover it?",
    "Discard", "Recover");
```

The first button is the default in a `BAlert` (rightmost, Enter), and here it
is **Discard** — the one choice that throws the recovered work away. The
question is asked at the worst moment for a reflex: the user has just restarted
after a crash, and pressing Enter destroys the thing the dialog was raised to
save. Swap the order so Recover is the default and Discard takes the escape
shortcut. *Router:* **T15** (worklist item 19, trivial). *Severity:* Low, but
in the data-safety direction.

**K-5. "Windows to allow the user to change various program settings … the most
common mistakes are poorly-chosen defaults, inappropriate language for the
target audience, and too many choices … When possible, have changes take place
immediately instead of requiring the user to click OK."** (11, *Settings Window
Design*)

**Status: Not applicable today — and owned.** There is no settings window:
buffer size, count-in, monitor input and the export choices live in menus and
dialog state (`src/ui/MainWindow.cpp:362-395`). It is not an oversight —
`docs/PLAN_1.0.md` M3.5 specifies the window and lists what moves into it, and
the two things the HIG warns about are already handled in the meantime (the
radio menus mark the current value, and changes apply on the next Play rather
than at OK). *Router:* **T6**.

**K-6. "Windows used to search for data in a document … It only needs to be
shown when the user needs to set the search terms."** (11, *Find Windows*)

**Status: Not applicable.** No find window.

---

# Part L — HIG 12, Controls

**L-1. "Use GetPreferredSize and ResizeToPreferred … Leave sufficient padding
around them … Don't: Make them too small or ridiculously large … Leave them
blank … Change the label font."** (12, *Buttons*)

**Status: Met.** Buttons are laid out by the Layout Kit and sized from their
label (`src/ui/MainWindow.cpp:142-152`, `HeaderButton`, which calls
`GetPreferredSize` and then applies a `Themed` floor); padding comes from the
builder's `SetInsets`/spacing (`src/ui/ExportProgressWindow.cpp:27-33`); no
blank button exists; the label font is never changed, only the colour through
the theme (`DawControlLook`, `docs/agent-prompts/21-widget-kit-PR.md`).

**L-2. "Label checkboxes so that the user understands ahead of time … Use them
for choosing one item from a list. Use a group of radio buttons instead …
Use more than 5 or 6 in a group."** (12, *Checkboxes* / *Radio Buttons*)

**Status: Met.** Every `DawCheckBox` carries a label; exclusive choices are
menus in radio mode or `DawMenuField`s, not checkboxes; no radio group exceeds
five (the buffer-size menu lists five). Radio menus set a default and mark it
(`MarkRadio`, `src/ui/MainWindow.cpp:2789-2796`).

**L-3. "Control groups … are for visually associating different controls which
work together for a common function … Don't: Nest control groups"** (12,
*Control Groups*)

**Status: Not applicable.** No `BBox` is used anywhere in `src/ui/`; grouping
is done with header strips and panel colours instead
(`PaneHeader`, `docs/UI_GUIDELINES.md` §4).

**L-4. "Label the ends of the slider … Place the largest value at the right or
top … Don't: Use a slider for a progress indicator or scrollbar."** (12,
*Sliders*)

**Status: Met.** Sliders and faders are `DawSlider`/`DawKnob`/the mixer fader,
all scale-aware, none used as a progress indicator (progress is a
`BStatusBar`, L-9). The mixer strip prints the value it is editing immediately
beside the control (`src/ui/MixerWindow.cpp:227`, `:252`). Whether every slider
prints both ends of its range is a looking-at-the-screen question and belongs
to the `DAW_UI_SHOTS` pass; nothing in the code contradicts the rule.

**L-5. "Sort your list. Don't: Try to use it to select from hundreds of items.
Use a search instead. Use list items to toggle options like a list of
checkboxes."** (12, *Lists*)

**Status: Deviates — Low.** The sample browser fills a `BListView` from a
`BQuery` and never sorts it: the query has a name filter but no
`PushAttr`/`Sort` (`src/ui/SampleBrowser.cpp:108-140`), so rows arrive in
filesystem order while each row already displays the duration and BPM the
browser reads from attributes — data the user would obviously want to sort by.
(The 500-entry fetch cap at `:118` is the HIG's "hundreds of items" ceiling,
correctly applied.) Neither list uses checkboxes as toggles, and both use
single selection. *Router:* **T15** (worklist item 25, trivial/small) — sort by
name at minimum; by duration/BPM is the obvious follow-up and is host-testable
as a comparator. *Severity:* Low.

**L-6. "Use tabs to manage and organize otherwise complex dialog windows …
Don't: Use tabs to select a particular mode / Nest tab views / Use one tab all
by itself / Use unlabelled icons for tab titles."** (12, *Tabs*)

**Status: Met — and it was fixed deliberately.** No `BTabView` exists in
`src/`; the one that did (a single unlabelled tab holding the bottom pane) was
removed in the M1.4 visual pass, and the replacement is a header strip:

```
src/ui/MainWindow.cpp:490-491
// No tab bar while there is only one thing to show -- a lone unlabelled tab
// was all the old BTabView added.
```

This is the "Don't: Use one tab all by itself / unlabelled icons" pair
satisfied by deletion. The docked pane's pages (T2) are specified as a
segmented control rather than tabs for the same reason
(`docs/agent-prompts/24-continue-1.0-ENTRY.md` T2; `docs/UI_GUIDELINES.md` §4).

**L-7. "Adjust range and step size when their target view is resized. Don't:
Nest BScrollViews."** (12, *Scrollbars*)

**Status: Met.** Three `BScrollView`s exist, one per scrolling view, none
nested (`src/ui/PluginBrowser.cpp:74`, `src/ui/EffectsWindow.cpp:1218`,
`src/ui/SampleBrowser.cpp:76`); the mixer takes its horizontal scrolling from
the split view. Ranges are recomputed on resize where the content is custom
drawn (the timeline's scroll range follows the project length).

**L-8. "Make pop-up menus sticky. Use pop-up menus for context menus. Don't:
Use the primary mouse button to show a context menu."** (12, *Pop-up Menus*)

**Status: Met.** Context menus are opened with the secondary button on the
timeline (ruler tempo/meter, clip menu) and on clip edges, never with the
primary button, and no context menu is triggered by clicking a button.

**L-9. "BStatusBars are used to show progress … Do them whenever an operation
has the potential to take a while … Properly label the progress bar."** (12,
*Progress Meters*)

**Status: Met.** `src/ui/ExportProgressWindow.cpp:23-24` — a `BStatusBar` with
a label naming the job, updated from the render thread through the pulse, plus
a working Cancel (`:29-31`). Export, stems, freeze and region renders all run
as `RenderJobs` and report.

**L-10. "Use them to speed up access to common functions / Use icons from the
standard set if possible / Make the function of each button obvious from the
icons used / Don't: Add a toolbar just for looks / too many buttons"** (12,
*Toolbars*)

**Status: Met.** The transport bar and the tool palettes carry only functions
that are one-click common, each glyph is drawn to the app's own hand
(`src/ui/widgets/DawIcons.h`) and each carries a hover tooltip so the function
is obvious without a legend, and there is no decorative toolbar. Icons are not
from a system set because Haiku ships single-colour mini icons at 16 px rather
than the transport glyphs a DAW needs; the same chapter's icon-geometry rules
are what T12 applies.

---

# Part M — Coding Guidelines *(upstream only)*

**Read this part as a condition of a possible upstream contribution, not as a
defect list.** The guidelines are explicit that they govern patches to Haiku
("*Code which doesn't follow the guidelines below will not be accepted*") and
that their first rule is consistency with the surrounding code. Our files are
consistent with each other; they are not written in Haiku's house style. The
counts in the summary separate these six from the app-facing findings for that
reason.

**M-1. "If you will be contributing code or patches to Haiku, you will need to
strictly follow the code style guidelines."** (preamble)

**Status: Not applicable — upstream only.** For files inside this repository,
the same document's *"Make your code not stick out — make it consistent with
the rest of the code you are contributing to"* resolves to this repository's
style, which `AGENTS.md` also states. Nothing here needs changing unless the
code is offered upstream; M-2 through M-6 are what that would cost.

**M-2. "In header files, there is no blank line between the license text and
the header guard … the guard name follows the `THIS_CLASS_H` pattern, with an
inline `#endif // THIS_CLASS_H` comment."**

**Status: Deviates — upstream only.** All 110 headers use `#pragma once`; none
uses an include guard (`grep -rl "#pragma once" src/ | wc -l` → 110;
`grep -rl "#ifndef.*_H$" src/ | wc -l` → 0). Mechanical: one guard pair and one
closing comment per header, scriptable. *Router:* **upstream only**.

**M-3. "Prefer C-style headers (string.h, stdlib.h) over their C++ equivalents
(cstring, cstdlib): Haiku's headers are generally C++ safe, and don't need this
workaround."; "Alphabetize #include statements, group "includes" and
<includes> separately."**

**Status: Deviates — upstream only.** 146 `#include <c…>` lines exist
(`grep -rn "#include <c[a-z]*>" src/ | wc -l`). Our own include order is
already grouped and alphabetical; the header *names* are the C++ spellings the
guideline rejects. Mechanical. *Router:* **upstream only**.

**M-4. "Use tabs to indent blocks."; "Set your editor to 4 spaces per tab …";
"A line must not have more than 100 columns."; long function definitions put
the return type on its own line with the opening brace on the next.**

**Status: Deviates — upstream only.** No file in `src/` contains a tab
(`grep -rlP "^\t" src/ | wc -l` → 0; 172 files indent with four spaces), 197
lines exceed 100 columns, and definitions such as
`bool MainWindow::QuitRequested() {` (`src/ui/MainWindow.cpp:3126`) put the
brace on the signature line where Haiku puts it on the next. *Router:*
**upstream only** — a whole-tree reflow, and the one item here that is not
mechanical.

**M-5. "Put exactly two blank lines between functions … Include a newline at
every file end. Clean up trailing whitespace."**

**Status: Deviates — upstream only.** Functions are separated by one blank
line (the local convention), and trailing whitespace is not policed by a hook.
Ends of file are fine. *Router:* **upstream only**.

**M-6. "The preferred format for source file copyright and license notices
is: … Copyright … Distributed under the terms of the MIT License."**

**Status: Deviates — upstream only — and required.** No file in `src/` carries
a copyright or licence header (`grep -rl "Copyright" src/ | wc -l` → 0); the
licence lives only in `LICENSE` at the repo root (MIT, *Copyright (c) 2026
Marc Korte*). For a file to be accepted upstream, each one needs the block, and
the guideline specifies the spacing after it (two blank lines). *Router:*
**upstream only** — but note it would also be the honest thing to add if the
project ever wants per-file provenance.

**M-7. "Member variables start with f"; "Constants start with a k"; "Private
methods are prefixed with an underscore"; "All identifiers, names and comments
should be in US English."**

**Status: Met.** The convention is already ours: `fProject`, `fDoc`,
`fTitleShown` (`src/ui/MainWindow.cpp:2383`), `kMaxRecent`
(`src/app/AppSettings.h:33`), `kDesignFontSize` (`src/ui/Theme.h:25`),
`kInvalidTrackId`, `_PrivateMethod`-style underscore helpers in the model, and
US spellings throughout (`color`, not `colour`; `ColText`,
`TrackColor`). The code would pass this section unchanged.

**M-8. "Prefer using an Haiku API utility call over 'rolling your own'.
Prefer BString over malloc, strdup, free … Prefer BObjectList over BList …
Prefer using types defined in SupportDefs.h such as int32, uint32, over int,
long."**

**Status: Deviates — upstream only.** A deliberate architectural split: the
model, DSP and app layers are kit-free (`std::string`, `std::vector`,
`int64_t`) *so that they can be host-tested on Linux* — the rule this project
holds above all others (`AGENTS.md`, *Kit-free code is host-tested*). Only
`src/ui/` and the plugins use Haiku types, and there they use them properly
(`BString`, `BMessenger`, `status_t`, `int32`). Upstreaming would mean
re-platforming the kit-free core onto `BString`/`BObjectList`, which would
cost the host suite. *Router:* **upstream only**, and if this code is ever
offered upstream, offer `src/ui/` and the plugins — not the kit-free core.

---

# Part N — Prepare for publishing

**N-1. "Translating your application using catkeys files."**
<https://www.haiku-os.org/development/apps-prepare>

**Status: Deviates — Low/Medium.** No Localization Kit use at all: no
`BCatalog`, no `B_TRANSLATE`, no `catkeys`, no `Makefile` target for one
(`grep -rn "BCatalog\|B_TRANSLATE\|catkeys" src/ CMakeLists.txt` → nothing).
Every user-visible string is a literal in `src/ui/`. The work is not small —
there are several hundred literals and the layout is `Themed()`-scaled, which
interacts with translated string widths — but none of it is hard, and it is the
first item on the page's publication checklist. Worth deciding explicitly:
either localisation is in 1.0's scope or it is a documented post-1.0 item.
*Router:* **T13** (M8, and it is a scope question for Marc — the same class as
the MP3 and FLAC decisions recorded in `08-release-1.0-ENTRY.md` §2). Worklist
item 27.

**N-2. "Create icons for your application. At the HVIF Store you'll find
hundreds of vector icons under free licenses that you can use directly or as a
starting point."**

**Status: Deviates — Low.** No icon, for app or project file, and the resource
file says so (`haiku-daw.rdef.in:1-7`). This is the second half of the
publication checklist and it is already planned, with the constraint recorded:
nothing on the development machine can author an HVIF (no Icon-O-Matic, no
libicon; `rc` compiles icons only from raw HVIF bytes), so the SVG→HVIF step
needs Marc on a Haiku machine. *Router:* **T12**. Worklist item 26.

---

# Part O — Icon Guidelines

The guidelines at <https://www.haiku-os.org/development/icon-guidelines> are
the *specification* for T12, so they are recorded here as the checklist that
work must satisfy rather than as conformance that can be judged now. Both
items are Not applicable until an icon exists, and both stop being so the day
T12 lands.

**O-1. "If a cube were sitting on the floor, the angles of its edges would be
defined as follows: the line where the left front side and the floor meet
would have an angle such that for every two pixels it goes to the right, it
goes one pixel down … Haiku icons use a main black outline at least around the
silhouette of the icon's shape. The outline is 1 pixel wide at 32x32 size …
There are two light sources illuminating a Haiku icon. The main light source
casts a bright neutral light from the top left … A shadow … they all need to be
the same direction: straight to the right … colors are always a mixture, never
pure red, green, or blue."**

**Status: Not applicable — no icon yet.** The checklist T12 must satisfy:
isometric-floor perspective with verticals straight and non-floor edges
slightly de-perspectived; key light from top left, a warmer ambient from the
opposite side; gradients derived from those lights, surfaces toward the viewer
lighter at the top; saturated but never pure dominant colours, pastel where
they fade out; a self-shadow and a floor shadow cast to the right, not cut off
at the icon's edge; a 1 px black outline on the silhouette, used sparingly
inside; details optimised for 32×32 and reduced with Min/Max LOD; and, per the
same document, *"all Haiku icons are designed to be culturally and ethnically
neutral."* *Router:* **T12**.

**O-2. "Some of the system icons use overlays to further differentiate between
file system objects … Since the overlays need to be reasonably clear at 16x16
icon size, they need to be flat symbols, much like labels … Usually, you should
avoid any detail not strictly necessary to help the meaning of an icon …
Icons are usually viewed at 32x32 or 16x16 size … There should not be any
detail that is not well seen at this size."**

**Status: Not applicable — no icon yet.** This is the item that governs the
*project-file* icon specifically: it should read as a document at 32×32 with a
flat overlay mark distinguishing `.dawproj`, and every detail must survive
16×16 in Tracker. *Router:* **T12** — T12's definition of done already includes
checking both icons at 16, 32 and 64 px in Tracker and Deskbar
(`docs/agent-prompts/24-continue-1.0-ENTRY.md` T12).

---

# What we would have to change before offering any of this code to Haiku itself

For the record, so the question has an answer rather than a guess. Nothing
below is a defect in a third-party application; all of it is a condition of a
patch to the Haiku tree.

1. **The kit-free core is not Haiku-shaped and should not be offered as-is.**
   `src/model/`, `src/dsp/`, `src/midi/`, `src/storage/` and much of
   `src/app/` use `std::string`, `std::vector`, `int64_t` and the C++ standard
   library rather than `BString`, `BObjectList`, `int32` and `status_t`. That
   is the design that makes 55 test binaries run on a Linux host
   (`AGENTS.md`), and Haiku's guideline M-8 asks for the opposite. A
   contribution should be `src/ui/`, `src/plugin/` and the add-ons — the Haiku
   API surface — with the core left behind, or the core re-platformed as a
   separate, deliberate effort.

2. **House style (M-2 to M-6).** Include guards instead of `#pragma once`
   (110 headers); `<string.h>` instead of `<cstring>` (146 includes); tabs
   instead of four spaces and ≤100 columns (197 over-long lines); the
   return-type-on-its-own-line brace style for out-of-class definitions;
   exactly two blank lines between functions; a copyright block and MIT licence
   header on every file (none has one today).

3. **Haiku-defined types and utilities (M-8)** in the UI layer: `BString`
   `<<`/`SetToFormat` instead of `snprintf` into stack buffers, `BObjectList`
   where a `BList` or a hand-rolled vector of view pointers is in play,
   `AutoLock` instead of any `Lock()`/`Unlock()` pair, and `status_t` for error
   returns.

4. **Constants and identifiers (M-7).** Our `k*` constants (`kMaxRecent`,
   `kDesignFontSize`, `kInvalidTrackId`, `kMsgDropMidi`) already follow the
   guideline's `k` convention, and the `fMember` prefix is used throughout —
   but the `MainWindow` message block is an upper-case `MSG_*` enum (opens at
   `src/ui/MainWindow.cpp:75`), which the same rule would have as `kMsg*`,
   and the two styles already coexist in the tree. Renaming them is mechanical
   but touches every UI file. The guideline's own note that the `k` rule *"is
   different from the standard Be API constant names"* also means any constant
   pairing with a `B_` constant needs a collision check.

5. **Dead code, debug output and comments (M, *Dead code and Debugging
   code*).** Several files carry `std::fprintf(stderr, …)` progress lines
   (`src/main.cpp:156`, `:245`, `src/ui/RenderJobs.cpp:100-110`). They are
   deliberate — the app reports on stderr where a test or an SSH session can
   read it, and `docs/UI_GUIDELINES.md` leans on it — but upstream they belong
   behind `#if DEBUG` or in `Debug.h`'s macros.

6. **Submission hygiene, not code.** An upstream patch carries no
   `Co-Authored-By:` or other AI-attribution trailer — this repo's rule
   (`AGENTS.md`) and the same one applies to anything sent to Haiku, whose own
   conventions for commit authorship and review are in *Pushing Patches to
   Haiku 101*.

None of this needs doing for the app to ship. It is what a future "offer the
UI layer upstream" decision would cost, and it is roughly the size of the whole
UI layer once more.

---

# Worklist

Ordered so that the cheap, self-contained items come first and nothing depends
on an unmerged branch it cannot have. Sizes: **trivial** (<1 h), **small**
(~½ day including its spec, tests and screenshots), **medium** (1–2 days),
**large** (>2 days). Every UI item carries the `docs/UI_GUIDELINES.md` §1
screenshot pass as a non-optional part of "done" — that is not repeated below.

| # | Item | Router | Size |
|---|---|---|---|
| 1 | **Program menu**: add Program with About <app name>… / Settings… / Quit (Cmd+Q, bottom, separator above); remove About from Help and Quit from File (I-3) | **T14** | small |
| 2 | **About window**: add author/company and copyright; button "Close" not "OK"; handle Cmd+W (G-2) | **T14** | small |
| 3 | **Help menu**: Help ▸ Open manual… opening `docs/USER_GUIDE.md` with the preferred app; Help ▸ Haiku DAW website; a real shortcuts window from the one key table (`feature/keymap-meters` has the pattern); add `--help`/`-h` printing the same table (I-8, F-7) | **T14** | small–medium |
| 4 | **Sentence-case the menu labels** (F-4): "Save as…", "Open recent", "Import audio…", "Export WAV…", "Zoom to fit", "Keyboard shortcuts", "Master effects…" — menus, the key table and the USER_GUIDE together, so they cannot drift | **T14** | small |
| 5 | **Menu-bar order**: Program, File, Edit, Track, View, Audio, Help (I-9) — land with item 1, one rebuild | **T14** | trivial |
| 6 | **Cmd+F off Zoom to Fit** (D-4) — either drop the accelerator or move it to a free key; **Marc decides** if Zoom keeps a one-key binding | **T14** / Marc | trivial |
| 7 | **Toggle-able Inspector (Cmd+I)** — flagged as a judgement call, not a defect (D-4) | **Marc's decision** | — |
| 8 | **Edit menu Cut/Copy/Paste/Delete/Select All/Duplicate + dynamic Undo/Redo labels** (I-5, I-6) — **already implemented on `feature/arrange-depth`**; merge, do not rebuild | **T4** | merge only |
| 9 | **Program ▸ Settings… + Cmd+,** opening the Preferences window (D-5, I-3) | **T6** | with M3.5 |
| 10 | **The Preferences window itself** — M3.5's list; it is also what makes item 9 possible | **T6** | with M3.5 |
| 11 | **`set_ui_color` removal and native-by-default look** (C-7) — **already implemented on `feature/theme-modes`**; merge, do not rebuild | **T1** | merge only |
| 12 | **`InspectorView.cpp:181` hardcoded font size 13** → `Themed(13.0f)` or `ThemeFontBold()` (F-5) | **T15** | trivial |
| 13 | **Cmd+wheel for zoom in the plan** (D-2) — M2.2 says Ctrl+wheel; the HIG gives zoom to Cmd+wheel. Edit the plan line before M2.2 is built | **T4** | trivial (plan) |
| 14 | **`CenterOnScreen()` for the first window; clamp a restored frame to `BScreen`** (J-4) | **T15** | small |
| 15 | **Initial size == `SetSizeLimits` minimum** (J-3) — measure the real minimum in the screenshot pass | **T15** | small |
| 16 | **`B_NOT_MINIMIZABLE` on the utility windows** (J-7), in one pass over the J-1 list | **T15** | small |
| 17 | **Ref filters on the file panels** (C-13) — Open Project, Import Audio, Import MIDI, Locate; permissive, per the HIG's warning about unfiled types | **T15** | small |
| 18 | **Drop feedback in `TimelineView`** (E-2) — lane highlight and clip drop line on `DragEnter`/`DragMoved`; say "ignored" in the window rather than only on stderr | **T4** (M2.1) | small |
| 19 | **Swap the Recover alert's buttons** so Recover is the default and Discard takes Escape (K-4) | **T15** | trivial |
| 20 | **Export-failure alert: say why, and what to try** (F-1) — pass the failure code through | **T15** | trivial |
| 21 | **Settings as a flattened `BMessage`** (C-2) — a Haiku-only `SettingsFile` behind the same struct, plus migration of an existing text file. Cost: one new file (~80 lines), the kit-free round-trip test moves to the VM, half a day | **Marc's decision** (→ **T6**) | medium |
| 22 | **Recent-files count: 10 or 5** (C-6) — the HIG contradicts itself (ch. 3 says 10, ch. 9 caps at 5); pick one and record why | **Marc's decision** | trivial |
| 23 | **Attach Open recent to the Open item; sentence-case the label** (C-6, F-4) | **T14** | trivial |
| 24 | **Put copied clips on the system clipboard as `entry_ref`s** (E-3) — a friendliness to other apps rather than a defect; the HIG's own example is audio too large to copy | **Marc's decision** | small |
| 25 | **Sort the sample browser list** (L-5) — name first; duration/BPM is the obvious follow-up and is host-testable | **T15** | small |
| 26 | **HVIF app icon and `.dawproj` file icon** to the O-1/O-2 checklist (G-1, N-2) — already planned and Marc-gated at the SVG→HVIF step | **T12** | large |
| 27 | **catkeys localisation** (N-1) — several hundred literals; a scope decision as much as a task | **T13** (Marc) | large |
| 28 | **Packaging gaps from `docs/PLAN_1.0.md` M8.1** — the Deskbar symlink, installed docs and templates, `lib:` REQUIRES, `rc`/`xres` in the recipe | **T13** | medium |
| 29 | **No busy cursor: move the soundfont decode off the Instrument window's looper** (H-1, C-3) — then delete both `SetCursor` calls; report progress through the pulse the way export does | **T15** | small–medium |
| 30 | **Upstream readiness**, if it is ever wanted: licence headers, include guards, C headers, tabs/columns/brace style, `BString`/`BObjectList` in the UI layer (Part M) | **upstream only** | large |

## Tasks this audit proposes

Written in the `§5` style of `docs/agent-prompts/24-continue-1.0-ENTRY.md` so
they can be adopted, renumbered or rejected without further editing. Each needs
its spec file (`docs/agent-prompts/NN-<slug>.md`) written before any code, per
that section's rule.

**T14 — HIG menus: Program, Help and the About window.** (`feature/hig-menus`)
Every finding this audit routes to a menu, in one branch, because they all land
in the same 70 lines of `MainWindow`'s constructor and splitting them means
rebuilding the menu bar three times and re-screenshotting three times.

- Add the **Program menu** (worklist 1) and rebuild the bar in the order
  Program, File, Edit, Track, View, Audio, Help (item 5).
- **About** (item 2): author/company and copyright lines; a "Close" button
  instead of "OK"; Cmd+W handled. Consider `BAboutWindow` — the HIG names it as
  the sufficient answer — or keep the alert and fix the four clauses by hand.
  If the Program menu lands, About moves there and the Help menu loses its only
  current item.
- **Help** (item 3): "Open manual…" opening `docs/USER_GUIDE.md` through the
  user's preferred application (the HIG's first choice) with a fallback
  read-only text view; a website item; and `--help`/`-h` on the command line
  printing the same key list, generated from one kit-free table.
  `feature/keymap-meters` (M3.2) already builds that table
  (`src/model/KeyMap.h`) and a real `ShortcutsWindow` — branch from it if it
  has merged by then, or co-ordinate the merge order, or this becomes a fourth
  copy of the key list.
- **Text conventions** (items 4 and 23): sentence-case every label; move
  Open recent under Open.
- **Cmd+F** (item 6) if Marc says yes.
- Done when: `ui_functional_tests` drives each new item by posted message,
  `Shot()` exists for the About and shortcuts windows, and every shot of the
  changed menus has been reviewed. This is a visible-surface change, so the
  screenshot rule in `docs/UI_GUIDELINES.md` §1 is the definition of done, not
  a step in it.

**T15 — HIG window, alert and panel conventions.** (`feature/hig-windows`)
The small, independent, non-menu items, all inside `src/ui/`:

- Window placement and size: `CenterOnScreen()` on first run and a
  `BScreen`-clamped restore (item 14); the minimum size the window actually
  needs, equal to the initial size (item 15); `B_NOT_MINIMIZABLE` on the
  utility windows (item 16).
- Alerts: the Recover default (item 19), the export failure's reason and advice
  (item 20).
- The file panels' ref filters (item 17).
- `InspectorView`'s hardcoded font size (item 12).
- The sample browser's sort order (item 25).
- **The busy cursor** (item 29) — the one item here that is not mechanical:
  move `SoundfontCache::Load` off the window thread, report through the pulse,
  and delete both `SetCursor` calls. The HIG's own remedy is the fix; the
  cursor is the symptom. If the decode genuinely cannot leave the thread, the
  cursor still goes — "there is not nor will there ever be a busy cursor" — and
  what replaces it is a determinate or indeterminate progress indicator inside
  the window, which is where the user is looking anyway.
- Done when: the screenshot pass covers every changed window at 100 % and
  150 % (`SetThemeScaleOverride(1.5f)`), a fresh-profile run and a run with a
  restored frame from a larger screen are both checked, and the filter panels
  are opened on a directory containing both matching and non-matching files.
