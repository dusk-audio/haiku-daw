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

#include <Messenger.h>
#include <Window.h>

#include <map>
#include <utility>
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
    void StartCapture();             // open the Recorder (after any count-in)
    bool StartRecordEngine(Frame engineStart);   // engine for overdub monitoring
    void UpdatePulse();              // run the poll iff playing or recording
    void UpdateTimeReadout(Frame playhead);
    void UpdateLoudnessReadout(float momLufs, float shortLufs, float truePeakDb);
    void PushTrackPeaks();           // engine per-track peaks -> timeline meters
    void SaveTo(const char* path);
    void LoadFrom(const char* path);
    void LoadSettings();             // ~/config/settings/HaikuDAW/settings
    void SaveSettings();
    void ImportAudio(const char* path);   // add a WAV as a clip
    void RebuildPeaks();             // rebuild waveform envelopes after load

    Project*        fProject;        // non-owning (the session)
    CommandStack*   fStack;          // non-owning
    PeakMap*        fPeaks;          // non-owning; new takes add entries here

    TimelineView*   fTimeline;
    class TransportBar* fTransport = nullptr;
    BStringView*    fTimeView;
    BStringView*    fLoudView = nullptr;   // LUFS / true-peak readout
    MeterView*      fMeter;
    BSlider*        fMaster;
    BTextControl*   fTempo;

    std::unique_ptr<Engine>   fEngine;    // rebuilt each Play
    std::unique_ptr<Recorder> fRecorder;  // active while recording
    BMessageRunner*           fPulse = nullptr;  // 60 Hz UI poll
    BMessageRunner*           fAutosave = nullptr;  // periodic crash-recovery save
    BMessenger                fMixerMsgr;    // open mixer window (for live peaks)
    BFilePanel*               fSavePanel = nullptr;
    BFilePanel*               fOpenPanel = nullptr;
    BFilePanel*               fExportPanel = nullptr;
    BFilePanel*               fImportPanel = nullptr;
    BMenuItem*                fMetItem = nullptr;   // metronome toggle
    BMenuItem*                fDimItem = nullptr;   // monitor dim toggle
    BMenuItem*                fMonoItem = nullptr;  // monitor mono toggle
    BMenu*                    fBufMenu = nullptr;   // buffer-size submenu (for marks)
    size_t                    fBufferFrames = 512;  // output buffer frames/channel
    bool                      fMetronome = false;
    bool                      fMonDim = false;
    bool                      fMonMono = false;
    bool                      fPlaying = false;
    bool                      fRecMode = false;      // engine running for a take
    bool                      fCapturePending = false; // in count-in, not yet capturing
    bool                      fMonitorInput = false; // hear live input while armed
    bool                      fLoopRecord = false;   // capturing stacked takes over a loop
    int                       fTakeGroup = 0;        // running take-group id
    int                       fCountInBars = 0;      // metronome bars before capture
    BMenu*                    fCountInMenu = nullptr; // radio submenu (for marks)
    BMenuItem*                fMonInItem = nullptr;   // input-monitor toggle
    int                       fTakeCounter = 0;
    std::vector<TrackId>      fRecTracks;   // all armed targets for the take
    Frame                     fRecStart = 0; // frame the capture (clip) begins at
    Frame                     fRecPoint = 0; // record start (== fRecStart)
    std::string               fLastDir;      // last Open/Save/Import directory
    std::string               fTakeDir;      // where recorded takes are written
    std::string               fTakePath;     // full path of the current take
};

} // namespace daw
