// MainWindow — top-level DAW window.
//
// Layout: a transport bar (play/stop + time readout) across the top, the
// timeline view filling the rest. Owns the playback Engine: Play builds it
// from the current Project and starts it (rebuild-on-play — RT-safe, matches
// how the engine already loads a session); a BMessageRunner polls the
// engine's atomic playhead ~60 Hz and drives the timeline's playhead line.
//
// Track-header controls + command/undo wiring arrive in M4e.
#pragma once

#include "../model/Project.h"
#include "../model/PeakCache.h"
#include "../model/Command.h"
#include "../engine/Engine.h"
#include "../engine/Recorder.h"

#include <Window.h>

#include <map>
#include <memory>
#include <string>
#include <vector>

class BButton;
class BStringView;
class BMessageRunner;
class BFilePanel;
class BSlider;
class BMenuItem;
class BTextControl;
class BMenu;

namespace daw {

class TimelineView;
class MeterView;

class MainWindow : public BWindow {
public:
    // Like TimelineView: BWindow::Frame() would shadow the model's frame type
    // for every unqualified `Frame` in this class. A member typedef hides it.
    using Frame = daw::Frame;

    using PeakMap = std::map<std::string, PeakCache>;
    MainWindow(BRect frame, Project* project, CommandStack* stack,
               PeakMap* peaks);
    ~MainWindow() override;

    void MessageReceived(BMessage* msg) override;
    bool QuitRequested() override;   // quit the app when the window closes

private:
    void StartPlayback();
    void StopPlayback();
    void StartRecording();
    void StopRecording();
    void UpdatePulse();              // run the poll iff playing or recording
    void UpdateTimeReadout(Frame playhead);
    void SaveTo(const char* path);
    void LoadFrom(const char* path);
    void ImportAudio(const char* path);   // add a WAV as a clip
    void RebuildPeaks();             // rebuild waveform envelopes after load

    Project*        fProject;        // non-owning (the session)
    CommandStack*   fStack;          // non-owning
    PeakMap*        fPeaks;          // non-owning; new takes add entries here

    TimelineView*   fTimeline;
    BStringView*    fTimeView;
    MeterView*      fMeter;
    BSlider*        fMaster;
    BTextControl*   fTempo;

    std::unique_ptr<Engine>   fEngine;    // rebuilt each Play
    std::unique_ptr<Recorder> fRecorder;  // active while recording
    BMessageRunner*           fPulse = nullptr;  // 60 Hz UI poll
    BFilePanel*               fSavePanel = nullptr;
    BFilePanel*               fOpenPanel = nullptr;
    BFilePanel*               fExportPanel = nullptr;
    BFilePanel*               fImportPanel = nullptr;
    BMenuItem*                fMetItem = nullptr;   // metronome toggle
    BMenu*                    fBufMenu = nullptr;   // buffer-size submenu (for marks)
    size_t                    fBufferFrames = 512;  // output buffer frames/channel
    bool                      fMetronome = false;
    bool                      fPlaying = false;
    int                       fTakeCounter = 0;
    std::vector<TrackId>      fRecTracks;   // all armed targets for the take
    Frame                     fRecStart = 0; // playhead at rec start
};

} // namespace daw
