# feat(export): the export dialog, and the bounce off the UI looper

Task R1. Branch `feature/export-dialog` off `master` `eafcfc7` (after package
07 merged).

Three commits: the exporter grows the surface the UI needs (`4dadb34`), the
dialog and the worker use it (`94a8836`), and the generic effects panel's wheel
edit is finally flushed before a save or a render (`05da5ef` — a pre-existing
bug this task's reconnaissance found next door). Section "What is verified" is
the honest split; the click list is at the end.

## What changed

| File | What |
| --- | --- |
| `src/engine/Exporter.h/.cpp` | `ExportFormat` (bit depth + dither), `ExportRange` (timeline window), `ExportJob` (progress + cancel) and one `ExportOptions` carrying them. Temp+rename write, chunked final write, progress reporting through one monotone path. Stems take the same options and report 1/N each. |
| `src/ui/ExportWindow.h/.cpp` (new) | The dialog: stems/mixdown, range, sample rate, bit depth, dither, normalization (target LUFS, ceiling dBTP, limiter). Posts its choices; never touches the model. |
| `src/ui/ExportProgressWindow.h/.cpp` (new) | A `BStatusBar` and a Cancel button. Fed by MainWindow on the pulse; its Cancel raises the job's flag. |
| `src/ui/MainWindow.h/.cpp` | `OpenExportWindow`, `StartExport` (stop → flush → snapshot → worker), `FinishExport` (pulse: reap, report, alert on failure). The two menu items now open the dialog; the file panels are shown from the dialog's answer. |
| `src/app/AppSettings.h/.cpp` | The dialog's last choices (nine fields) persist; an old settings file keeps the defaults. |
| `src/ui/EffectsWindow.h/.cpp` | `kMsgFxPanelFlush`: the generic panel hands over a wheel edit still inside its 400 ms debounce, so a save/export/flush can apply it itself. |
| `tests/exporter_job_tests.cpp` (new) | 53 checks: progress monotonicity and the exactly-1.0 contract, cancel before/during the render and inside the write, the destination-untouched and no-`.part` guarantees, dither on/off, the range slice equalling the full bounce, stems format forwarding. |

## Semantics and decisions

**The thread model.** The codebase has no background reader of the Project
today: every render (export, freeze, autosave) runs on the MainWindow looper.
The worker renders a **snapshot** taken on the looper, after `StopPlayback()`
and `FlushFxEditors()` — the two steps the synchronous path already had, kept
in the same order because the flush is what makes the model hold the value the
user just heard (the exporter reads the model; a knob inside an editor's 400 ms
debounce is audible but not yet committed). `Exporter.cpp` already deep-copies
per stem for exactly this reason, so the precedent is the codebase's own.

**Ordering rules that are load-bearing.** The progress window is created AFTER
the flush and AFTER the snapshot but BEFORE the thread starts: after, so a
visible "exporting" can never precede a settled model; before, so a very short
export cannot finish and be handled while there is nothing to close.

**Progress and completion travel on the 60 Hz pulse.** The worker writes one
`std::atomic<float>` and two plain result fields; it publishes the results with
a release store to `fExportRunning` and the pulse reads them after an acquire
load. This is the pattern Recorder/WavWriter established (atomics for streamed
values), and it means the worker never posts a message or touches a Haiku
object.

**Cancel is a flag flip**, polled between blocks — and one pass cannot be
interrupted: the look-ahead limiter processes the whole buffer in one call. The
worst-case latency of a Cancel is one block (or the limiter's pass), which is
why the button does not need to disable itself.

**Temp+rename.** A cancelled or failed export leaves nothing at the destination
name, and an existing file keeps its old bytes (the writer used to truncate it
at open). This is a behaviour change for the failure case, and the improvement
is the point.

**Range.** Every project→output conversion subtracts the window start, so a clip
or note beginning before the window keeps its negative offset and its tail still
sounds. With the default range the added term is zero and the bounce is
byte-identical — pinned by comparing a whole-project bounce with an
explicitly-clamped one, byte for byte. That comparison is also what caught
`ToOut`'s rounding: `+0.5` then truncating rounds negative frames toward zero,
which shifted straddling material by a frame; it rounds properly now, which
changes nothing at or above zero.

**Why the numbers are text fields.** Haiku's `BSlider` draws only its label —
there is no value text (the header's `UpdateText()` is a getter whose backing
field nothing ever sets). A loudness target the user cannot read back is worse
than a text field, so target LUFS and ceiling dBTP are `BTextControl`s, parsed
with a range clamp and a fallback.

**Freeze did not move.** It renders one track and its result feeds a model
command that must run on the looper; moving it would need its own completion
path for a job short enough to block for. Left synchronous and said so, which
the task allows ("if it is cheap; otherwise leave it and say so").

## What is verified, and how

```
cmake --build build-host                       # exit 0
ctest --test-dir build-host                    # 50/50  (49 + exporter_job_tests)
./build-host/exporter_job_tests                # 53 checks, 0 failures
sh scripts/haiku_syntax_check.sh <changed UI>  # 0 FAIL
# On the VM (the tip with all three commits); both configurations, the daw
# binary links, configure and build exit 0:
ctest --test-dir build                         # 50/50   (LV2 on)
ctest --test-dir build-off                     # 46/46   (-DDAW_LV2=OFF)
```

**Mutation testing** — each mutation applied, the suite run, the code restored:

| Mutation | Result |
| --- | --- |
| no final progress report | caught (2 checks) |
| cancel flag ignored | caught (3) |
| cancelled export leaves the `.part` behind | caught (1) |
| no temp at all (straight to the destination) | caught (4) |
| dither flag ignored | caught (1) |
| range: clip placement not shifted | caught (1) |
| range: notes not shifted | caught (1) |
| stems ignore the format | caught (2) |
| stems: no final 1.0 report | caught (2) |
| stems: 1.0 reported even after a cancel | caught (1) |
| the monotone guard allows repeated values again | caught (1) |

Two of these were the tests' own fault first: the "cancel during the write"
case used a file that fitted in ONE write chunk (no middle to cancel in), and
the range test had no audio clip, so `PlaceClip`'s shift was never exercised.
Both fixed before the mutations were re-run.

One mutation is deliberately left uncaught: removing the between-stems cancel
check changes nothing observable (each stem's inner export refuses at its own
first check), it only skips a deep copy per remaining stem. The comment says so.

## The review finding on the stems' progress

A reviewer caught the job contract's sharp edge: `ExportStems` maps each stem
into its own band of 0..1, so when the LAST stem failed — an empty track has
nothing to render, and its export returns false before reporting anything — the
run finished with `written > 0` (a success for the caller) while the bar stopped
short of 100 %. `ExportJob` says progress reaches 1.0 exactly once, on success,
and that shape did not hold it.

Fixed: `ExportStems` now has its own monotone guard (the same `ExportRun` the
single export uses) and reports 1.0 after the loop when a stem was written and
the run was not cancelled. The guard drops REPEATED values as well as backwards
ones (`<=`, not `<`) — which is what keeps "exactly once" true in the common
shape, where the last stem succeeded and its own report already reached the end
of its band. Two test shapes came out of it, both mutation-checked; the first
attempt had only the empty-last-track shape, which cannot see a doubled 1.0.

The same finding reached into the UI: a stems run cancelled after some stems
were written still returns a count, so `FinishExport` now checks the cancel flag
BEFORE the written count — a cancel is a cancel, not "exported 2 stem(s)".

## What is NOT verified

| Claim | State |
| --- | --- |
| The dialog, the progress bar, the Cancel button, the panel ordering | **Compiles (cross-compiler); never clicked.** The click list below is the only way to know. |
| Anything audible | The export renders offline; "it sounds like playback" is unchanged from before this branch and is R5's business. |

### Click list

Run on the VM from the checkout synced to this branch (`~/haiku-daw` once the
package-04 click test is done — say so and it gets synced and rebuilt; the
branch is `feature/export-dialog`).

1. File ▸ Export WAV… → the dialog appears with the last-used choices.
   -> Expected: Cancel asks for nothing; Export… asks for a file name.
   ✗ Failure: the file panel appears before the dialog, or the dialog's Cancel
   still opens a panel.
2. In the dialog: bit depth 24-bit, tick "Normalize loudness", target -14, press
   Export…, pick a name.
   -> Expected: a progress window appears, the bar advances smoothly to 100 %,
   the window closes by itself, and the file exists and plays.
   ✗ Failure: the main window freezes (the work is still on the looper); the bar
   never moves; no file; a `.part` file left next to the chosen name.
3. Repeat with a long project and press **Cancel** mid-bar.
   -> Expected: the bar stops, the window closes within a moment, no file is
   created (or an existing file at that name keeps its old contents), and no
   `.part` file is left behind.
4. Export again with the stems box ticked.
   -> Expected: a folder-named panel; one file per non-bus track inside, named
   `NN_<track>.wav`; the progress spans all stems and reaches 100 %.
5. Set the range to "Loop range" with the transport loop over part of the
   project, export, and compare with a whole-project export.
   -> Expected: the file starts at the loop start and is the loop's length.
6. During an export, keep working: move the playhead, drag a fader, undo.
   -> Expected: the window stays responsive; the export is unaffected (it renders
   the snapshot taken when it started).
7. Open an LV2 editor, move a knob, and while it is still "warm" (within about
   half a second) start an export.
   -> Expected: the exported file contains the new value (the flush runs before
   the snapshot).
8. Set the dialog's choices, quit the app, relaunch, open the dialog.
   -> Expected: the last choices are back.
9. In the generic effects panel (a built-in insert, not an LV2 one), move a knob
   with the mouse WHEEL, then hit Cmd-S at once and reopen the project.
   -> Expected: the project holds the value you wheeled to. (This is the flush
   above; Cmd-S is the only entry point fast enough for a human to beat the
   400 ms debounce — the export flow now takes longer than that by itself.)

## The neighbouring bug the reconnaissance found, fixed here (`05da5ef`)

`FlushFxEditors()` only walked the native-editor watch table. The generic
parameter panel folds wheel notches into one undo step behind its own 400 ms
timer, and its commit is an **async post** to MainWindow — which a flush cannot
wait for, because its callers read or serialize the model the moment it returns.
A wheel edit inside that window was therefore missing from the save, the
autosave, the frozen render and the bounce: the audio had it (the live channel
is immediate), the file did not. Pre-existing — it affected `SaveTo` and
autosave identically — and fixed here because R1's flush guarantee would
otherwise be false for one of the two editors in the app.

The fix follows the native-editor flush's shape but not its mechanism: the panel
is asked (`kMsgFxPanelFlush`) and ANSWERS with the payload `kMsgApplyFx` would
have carried, dropping its own timer so it cannot post the same edit a second
time as its own undo step; MainWindow applies that answer synchronously, exactly
as `kMsgApplyFx` does. Both timeouts are bounded (200 ms each), the lesson from
package 07's third review.

Two limits, stated: only the panel MainWindow is tracking (`fFxMsgr`, the
last-opened one — the same single slot the live meters already use) is asked, and
a panel whose timer fires in the sliver before it services the flush would apply
the edit twice — the same residual race the native-editor flush documents, and
harmless (the second apply writes the same chain).

## Known, not fixed
- The progress window has no "finished" state and no way to re-open it; a second
  export while one runs is ignored (`StartExport` returns early). The dialog's
  Export button is the only entry point, and the file panel covers it.
- Freeze keeps its hard-coded 16-bit/project-rate render (see above).
- The dialog writes no `.part`-style atomic anything of its own: the destination
  is asked for with the cached `BFilePanel`s, which never had a default
  directory. Pre-existing, unrelated to the worker.
