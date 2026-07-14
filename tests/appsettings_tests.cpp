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

    AppSettings b;
    CHECK(b.Deserialize(a.Serialize()));
    CHECK(b.bufferFrames == 1024);
    CHECK(b.countInBars == 2);
    CHECK(b.metronome == true);
    CHECK(b.monitorInput == true);
    CHECK(b.lastDir == "/boot/home/My Samples");
    CHECK(std::fabs(b.winL - 10) < 1e-3 && std::fabs(b.winR - 900) < 1e-3);

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
