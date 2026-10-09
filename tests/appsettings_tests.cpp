// Host tests for AppSettings text (de)serialization round-trip.
#include "../src/app/AppSettings.h"

#include <cmath>
#include <cstdio>

static int g_checks = 0, g_fails = 0;
#define CHECK(cond)                                                       \
    do { ++g_checks; if (!(cond)) { ++g_fails;                            \
        std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); }   \
    } while (0)

using namespace daw;

int main() {
    AppSettings a;
    a.bufferFrames = 1024;
    a.countInBars  = 2;
    a.metronome    = true;
    a.monitorInput = true;
    a.lastDir      = "/boot/home/My Samples";   // note the space
    a.winL = 10; a.winT = 20; a.winR = 900; a.winB = 640;
    a.exportBitDepth     = 24;
    a.exportDither       = false;
    a.exportSampleRate   = 96000;
    a.exportNormalize    = true;
    a.exportTargetLufs   = -16.5f;
    a.exportTruePeakCeil = -1.5f;
    a.exportLimiter      = true;
    a.exportRange        = 1;
    a.exportStems        = 1;

    AppSettings b;
    CHECK(b.Deserialize(a.Serialize()));
    CHECK(b.bufferFrames == 1024);
    CHECK(b.countInBars == 2);
    CHECK(b.metronome == true);
    CHECK(b.monitorInput == true);
    CHECK(b.lastDir == "/boot/home/My Samples");
    CHECK(std::fabs(b.winL - 10) < 1e-3 && std::fabs(b.winR - 900) < 1e-3);
    // The export dialog's last choices survive too (they are the reason the
    // dialog does not have to be re-specified on every bounce).
    CHECK(b.exportBitDepth == 24);
    CHECK(b.exportDither == false);
    CHECK(b.exportSampleRate == 96000);
    CHECK(b.exportNormalize == true);
    CHECK(std::fabs(b.exportTargetLufs - (-16.5f)) < 1e-3);
    CHECK(std::fabs(b.exportTruePeakCeil - (-1.5f)) < 1e-3);
    CHECK(b.exportLimiter == true);
    CHECK(b.exportRange == 1);
    CHECK(b.exportStems == 1);

    // An old settings file (no export keys at all) keeps the dialog defaults.
    AppSettings old;
    CHECK(old.Deserialize("buffer 512\nmetronome 0\n"));
    CHECK(old.exportBitDepth == 16 && old.exportDither == true);
    CHECK(old.exportSampleRate == 0 && old.exportNormalize == false);
    CHECK(old.exportRange == 0 && old.exportStems == 0);

    // Garbage / empty -> false, defaults retained.
    AppSettings c;
    CHECK(!c.Deserialize(""));
    CHECK(c.bufferFrames == 512);

    // Unknown keys ignored; known ones still parsed.
    AppSettings d;
    CHECK(d.Deserialize("mystery 5\nbuffer 256\n"));
    CHECK(d.bufferFrames == 256);

    // Regression: a malformed numeric value must keep the default (not zero it,
    // which would make buffer=0 -> div-by-zero in the audio path).
    AppSettings e;
    CHECK(e.Deserialize("buffer notanumber\ncountin 1\n"));
    CHECK(e.bufferFrames == 512);   // default retained
    CHECK(e.countInBars == 1);      // the good line still parsed

    std::printf("\n%d checks, %d failures\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
