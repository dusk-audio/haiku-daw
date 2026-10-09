// End-to-end host test for plugin-delay compensation in the offline exporter.
//
// Installs a test "plugin" that is a pure integer-sample delay reporting
// IEffect::LatencySamples() == its delay, then bounces impulse tracks through it
// and asserts the exporter's PDC makes the latency transparent (the impulse
// stays at its authored frame) and delay-aligns a dry sibling against it (their
// impulses land on the same output frame and sum). Uses the EffectType::Plugin
// factory hook (SetPluginFactory), so no real DSP effect needs to report latency.

#include "../src/engine/Exporter.h"
#include "../src/engine/WavSource.h"
#include "../src/engine/WavWriter.h"
#include "../src/dsp/EffectFactory.h"
#include "../src/dsp/IEffect.h"
#include "../src/model/Effect.h"
#include "../src/model/Project.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

static int g_checks = 0, g_fails = 0;
#define CHECK(cond)                                                       \
    do { ++g_checks; if (!(cond)) { ++g_fails;                            \
        std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); }   \
    } while (0)

using namespace daw;

// A test effect: an exact N-sample stereo delay that reports N samples of
// latency. Stands in for any latent plugin (a look-ahead limiter, a linear-phase
// FIR) that PDC must compensate.
class LatencyPlugin : public IEffect {
public:
    explicit LatencyPlugin(int n)
        : fN(n < 0 ? 0 : n), fBuf(static_cast<size_t>(fN) * 2, 0.0f) {}
    void Prepare(double) override {}
    void Process(float* s, int frames) override {
        if (fN == 0) return;
        for (int i = 0; i < frames; ++i) {           // read-then-write ring == N delay
            const float ol = fBuf[fW * 2 + 0], orr = fBuf[fW * 2 + 1];
            fBuf[fW * 2 + 0] = s[i * 2 + 0];
            fBuf[fW * 2 + 1] = s[i * 2 + 1];
            s[i * 2 + 0] = ol; s[i * 2 + 1] = orr;
            if (++fW >= static_cast<size_t>(fN)) fW = 0;
        }
    }
    void Reset() override { std::fill(fBuf.begin(), fBuf.end(), 0.0f); fW = 0; }
    int LatencySamples() const override { return fN; }
    const char* Name() const override { return "LatencyTest"; }
private:
    int                fN;
    std::vector<float> fBuf;
    size_t             fW = 0;
};

// Factory: "latency:N" -> an N-sample LatencyPlugin.
static std::unique_ptr<IEffect> TestPluginFactory(const std::string& name) {
    if (name.rfind("latency:", 0) == 0)
        return std::unique_ptr<IEffect>(new LatencyPlugin(std::atoi(name.c_str() + 8)));
    return nullptr;
}

static EffectDesc LatencyFx(int n) {
    EffectDesc d;
    d.type = EffectType::Plugin;
    d.pluginName = "latency:" + std::to_string(n);
    return d;
}

// Write a stereo 32-bit-float WAV: `total` frames of silence with a unit impulse
// at frame `at` (both channels).
static bool WriteImpulseWav(const std::string& path, int rate, int64_t total,
                            int64_t at) {
    std::vector<float> d(static_cast<size_t>(total) * 2, 0.0f);
    if (at >= 0 && at < total) { d[at * 2 + 0] = 1.0f; d[at * 2 + 1] = 1.0f; }
    WavWriter w;
    if (!w.OpenFormat(path, rate, 2, 32, /*floatOut=*/true)) return false;
    if (!w.WriteFloat(d.data(), d.size(), /*dither=*/false)) return false;
    return w.Close();
}

// Read a whole WAV back into an interleaved-stereo buffer.
static std::vector<float> ReadAll(const std::string& path, int64_t* frames) {
    std::vector<float> out;
    WavSource s;
    if (!s.Open(path)) { *frames = 0; return out; }
    const float* c = nullptr; size_t f = 0;
    while (s.ReadChunk(&c, &f)) out.insert(out.end(), c, c + f * 2);
    *frames = static_cast<int64_t>(out.size() / 2);
    return out;
}

// Frame index of the largest |left| sample.
static int64_t PeakFrame(const std::vector<float>& d) {
    int64_t best = -1; float bv = -1.0f;
    for (int64_t i = 0; i * 2 < static_cast<int64_t>(d.size()); ++i) {
        const float a = std::fabs(d[i * 2]);
        if (a > bv) { bv = a; best = i; }
    }
    return best;
}

// Largest |left| over frames [lo, hi).
static float MaxAbsIn(const std::vector<float>& d, int64_t lo, int64_t hi) {
    float m = 0.0f;
    lo = std::max<int64_t>(lo, 0);
    hi = std::min<int64_t>(hi, static_cast<int64_t>(d.size() / 2));
    for (int64_t i = lo; i < hi; ++i) m = std::max(m, std::fabs(d[i * 2]));
    return m;
}

int main() {
    SetPluginFactory(TestPluginFactory);

    const int    SR   = 48000;
    const int64_t P   = 5000;    // impulse timeline position
    const int64_t LEN = 2000;    // clip length
    const int     N   = 256;     // plugin latency

    const std::string wav = "/tmp/haiku_daw_pdc_impulse.wav";
    CHECK(WriteImpulseWav(wav, SR, LEN, 0));   // impulse at source frame 0

    // Build a one-audio-track project; the clip places the impulse at frame P.
    auto makeProject = [&](bool latent) {
        Project pr;
        pr.sampleRate = SR;
        pr.masterGain = 1.0f;
        Track t;
        t.id = pr.NextTrackId(); t.type = TrackType::Audio;
        t.gain = 1.0f; t.pan = 0.0f;
        Clip c;
        c.id = pr.NextClipId();
        c.startFrame = P; c.lengthFrames = LEN; c.sourceOffset = 0;
        c.sourcePath = wav;
        t.clips.push_back(c);
        if (latent) t.fx.push_back(LatencyFx(N));
        pr.AddTrack(t);
        return pr;
    };

    // --- Transparency: a latent track's impulse must land at the SAME output
    // frame as the dry render (PDC compensates the plugin's latency away).
    const std::string dryPath = "/tmp/haiku_daw_pdc_dry.wav";
    const std::string latPath = "/tmp/haiku_daw_pdc_lat.wav";
    std::remove(dryPath.c_str()); std::remove(latPath.c_str());
    CHECK(ExportWav(makeProject(false), dryPath, SR, {ExportFormat{32}}));
    CHECK(ExportWav(makeProject(true),  latPath, SR, {ExportFormat{32}}));

    int64_t nDry = 0, nLat = 0;
    std::vector<float> dry = ReadAll(dryPath, &nDry);
    std::vector<float> lat = ReadAll(latPath, &nLat);
    const int64_t pkDry = PeakFrame(dry);
    const int64_t pkLat = PeakFrame(lat);
    CHECK(pkDry >= 0 && pkLat >= 0);
    CHECK(std::llabs(pkDry - P) <= 1);                 // dry impulse at P
    CHECK(std::llabs(pkLat - pkDry) <= 1);             // latent == dry (transparent)
    // The impulse must NOT be shifted to P+N (the uncompensated position).
    CHECK(MaxAbsIn(lat, P + N - 2, P + N + 3) < 0.25f);
    // Exported length is unchanged by PDC (leading latency trimmed).
    CHECK(std::llabs(nLat - nDry) <= 1);

    // --- Sibling alignment: a latent track + a dry track, both with an impulse
    // at P, must land on the SAME output frame and sum (single peak ~2x), not
    // split into two peaks P and P+N.
    Project pr2;
    pr2.sampleRate = SR; pr2.masterGain = 1.0f;
    Track tl;   // latent
    tl.id = pr2.NextTrackId(); tl.type = TrackType::Audio;
    tl.gain = 1.0f; tl.pan = 0.0f;
    { Clip c; c.id = pr2.NextClipId(); c.startFrame = P; c.lengthFrames = LEN;
      c.sourcePath = wav; tl.clips.push_back(c); }
    tl.fx.push_back(LatencyFx(N));
    Track td;   // dry
    td.id = pr2.NextTrackId(); td.type = TrackType::Audio;
    td.gain = 1.0f; td.pan = 0.0f;
    { Clip c; c.id = pr2.NextClipId(); c.startFrame = P; c.lengthFrames = LEN;
      c.sourcePath = wav; td.clips.push_back(c); }
    pr2.AddTrack(tl); pr2.AddTrack(td);

    const std::string sibPath = "/tmp/haiku_daw_pdc_sibling.wav";
    std::remove(sibPath.c_str());
    CHECK(ExportWav(pr2, sibPath, SR, {ExportFormat{32}}));
    int64_t nSib = 0;
    std::vector<float> sib = ReadAll(sibPath, &nSib);
    const int64_t pkSib = PeakFrame(sib);
    CHECK(std::llabs(pkSib - P) <= 1);                 // aligned single peak at P
    // Two center-panned impulses summed ~= 2*0.7071 = 1.414; a single one ~0.7.
    CHECK(MaxAbsIn(sib, P - 1, P + 2) > 1.2f);         // they actually summed
    // No stray impulse at the uncompensated positions P+N (latent late) or
    // P-N (dry early) — alignment collapsed both onto P.
    CHECK(MaxAbsIn(sib, P + N - 2, P + N + 3) < 0.3f);
    CHECK(MaxAbsIn(sib, P - N - 2, P - N + 3) < 0.3f);

    // --- A zero-latency plugin exercises the Plugin path with pad == 0 (must be
    // identical to dry).
    Project pr0;
    pr0.sampleRate = SR; pr0.masterGain = 1.0f;
    Track t0; t0.id = pr0.NextTrackId(); t0.type = TrackType::Audio;
    t0.gain = 1.0f; t0.pan = 0.0f;
    { Clip c; c.id = pr0.NextClipId(); c.startFrame = P; c.lengthFrames = LEN;
      c.sourcePath = wav; t0.clips.push_back(c); }
    t0.fx.push_back(LatencyFx(0));
    pr0.AddTrack(t0);
    const std::string zeroPath = "/tmp/haiku_daw_pdc_zero.wav";
    std::remove(zeroPath.c_str());
    CHECK(ExportWav(pr0, zeroPath, SR, {ExportFormat{32}}));
    int64_t nZero = 0;
    std::vector<float> zero = ReadAll(zeroPath, &nZero);
    CHECK(std::llabs(PeakFrame(zero) - P) <= 1);

    // --- Pre-fader send from a latent node. Track A has a latency plugin AND a
    // pre-fader send (tapping BEFORE the plugin) into a unity bus B; both A and B
    // route to master. PDC must delay the pre-fader send by the plugin latency so
    // A's direct (post-plugin) output and its pre-fader send (via B) still land
    // on the same output frame and sum — not split into two peaks P and P+N.
    Project pr3;
    pr3.sampleRate = SR; pr3.masterGain = 1.0f;
    Track bus; bus.id = pr3.NextTrackId(); bus.type = TrackType::Bus;
    bus.gain = 1.0f; bus.pan = 0.0f;               // unity aux bus -> master
    Track ta; ta.id = pr3.NextTrackId(); ta.type = TrackType::Audio;
    ta.gain = 1.0f; ta.pan = 0.0f;
    { Clip c; c.id = pr3.NextClipId(); c.startFrame = P; c.lengthFrames = LEN;
      c.sourcePath = wav; ta.clips.push_back(c); }
    ta.fx.push_back(LatencyFx(N));
    ta.sends.push_back(Send{bus.id, 1.0f, /*preFader=*/true});
    pr3.AddTrack(bus); pr3.AddTrack(ta);
    const std::string prePath = "/tmp/haiku_daw_pdc_pre.wav";
    std::remove(prePath.c_str());
    CHECK(ExportWav(pr3, prePath, SR, {ExportFormat{32}}));
    int64_t nPre = 0;
    std::vector<float> pre = ReadAll(prePath, &nPre);
    CHECK(std::llabs(PeakFrame(pre) - P) <= 1);         // aligned single peak at P
    CHECK(MaxAbsIn(pre, P - 1, P + 2) > 1.2f);          // direct + pre-send summed
    CHECK(MaxAbsIn(pre, P + N - 2, P + N + 3) < 0.3f);  // no split at P+N

    // --- A REAL built-in latent effect: the live LookaheadLimiter (the first
    // built-in reporting non-zero latency). With the impulse under its ceiling it
    // is a pure La-frame delay, so PDC must make it transparent exactly as it does
    // the synthetic plugin — proof the generic LatencySamples() sum picks up a
    // real EffectDesc, not just the Plugin hook. 5 ms @ 48 k == 240 frames.
    const int LA = 240;
    Project prL;
    prL.sampleRate = SR; prL.masterGain = 1.0f;
    Track tL; tL.id = prL.NextTrackId(); tL.type = TrackType::Audio;
    tL.gain = 1.0f; tL.pan = 0.0f;
    { Clip c; c.id = prL.NextClipId(); c.startFrame = P; c.lengthFrames = LEN;
      c.sourcePath = wav; tL.clips.push_back(c); }
    tL.fx.push_back(LimiterDesc(0.0f, 5.0f, 60.0f, 0.0f));   // ceiling 0 dB
    prL.AddTrack(tL);
    const std::string realPath = "/tmp/haiku_daw_pdc_real.wav";
    std::remove(realPath.c_str());
    CHECK(ExportWav(prL, realPath, SR, {ExportFormat{32}}));
    int64_t nReal = 0;
    std::vector<float> real = ReadAll(realPath, &nReal);
    CHECK(std::llabs(PeakFrame(real) - P) <= 1);            // transparent: peak at P
    CHECK(MaxAbsIn(real, P + LA - 2, P + LA + 3) < 0.25f);  // not shifted to P+La
    CHECK(std::llabs(nReal - nDry) <= 1);                   // length unchanged by PDC

    std::remove(wav.c_str());
    std::remove(dryPath.c_str()); std::remove(latPath.c_str());
    std::remove(sibPath.c_str()); std::remove(zeroPath.c_str());
    std::remove(prePath.c_str()); std::remove(realPath.c_str());
    SetPluginFactory(nullptr);

    std::printf("exporter_pdc_tests: %d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
