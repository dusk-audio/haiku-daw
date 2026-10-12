// Host tests for the kit-free palette derivation (T1: theme modes).
//
// The point of these is the part a screenshot cannot check on every machine:
// the tokens derived in System mode have to read on a light AND a dark system
// panel, and the dark palette has to stay exactly what the app already looked
// like. Contrast is WCAG's ratio, asserted at 4.5:1 for text.
#include "../src/app/ThemePalette.h"

#include <cmath>
#include <cstdio>
#include <string>

static int g_checks = 0, g_fails = 0;
#define CHECK(cond)                                                       \
    do { ++g_checks; if (!(cond)) { ++g_fails;                            \
        std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); }   \
    } while (0)

using namespace daw;

// A named contrast assertion: prints the pair when it fails, so a light-panel
// regression says which token it was.
static void CheckContrast(const char* what, ThemeColor fg, ThemeColor bg,
                          float minRatio) {
    ++g_checks;
    const float ratio = ContrastRatio(fg, bg);
    if (ratio < minRatio) {
        ++g_fails;
        std::printf("  FAIL %s: contrast %.2f < %.2f  "
                    "(fg %u,%u,%u on bg %u,%u,%u)\n",
                    what, ratio, minRatio, fg.r, fg.g, fg.b, bg.r, bg.g, bg.b);
    }
}

// The dark-panel counterpart of Haiku's defaults (its dark Appearance).
static ThemeBase DarkSystemBase() {
    ThemeBase b;
    b.panel                  = Rgb8(43, 43, 43);
    b.panelText              = Rgb8(253, 253, 253);
    b.document               = Rgb8(0, 0, 0);
    b.documentText           = Rgb8(234, 234, 234);
    b.control                = Rgb8(29, 29, 29);
    b.controlText            = Rgb8(230, 230, 230);
    b.controlBorder          = Rgb8(195, 195, 195);
    b.menuBackground         = Rgb8(28, 28, 28);
    b.menuItemText           = Rgb8(255, 255, 255);
    b.menuSelectedBackground = Rgb8(90, 90, 90);
    b.menuSelectedItemText   = Rgb8(255, 255, 255);
    b.scrollBarThumb         = Rgb8(39, 39, 39);
    b.listBackground         = Rgb8(0, 0, 0);
    b.listSelectedBackground = Rgb8(90, 90, 90);
    b.listItemText           = Rgb8(255, 255, 255);
    b.listSelectedItemText   = Rgb8(255, 255, 255);
    b.keyboardNavigation     = Rgb8(0, 0, 229);
    return b;
}

// Everything a panel has to satisfy, whatever the base: the text tokens read,
// the playhead reads, and the grid is visible without being a stripe.
static void CheckReadable(const ThemeBase& base, const char* which) {
    const ThemeTokens t = DeriveTokens(base, ThemeMode::System);

    char what[128];
    std::snprintf(what, sizeof what, "%s: panelText on panel", which);
    CheckContrast(what, t.text, t.background, 4.5f);
    std::snprintf(what, sizeof what, "%s: panelText on header", which);
    CheckContrast(what, t.text, t.header, 4.5f);
    std::snprintf(what, sizeof what, "%s: documentText on a well", which);
    CheckContrast(what, t.lcdText, t.lcd, 4.5f);
    std::snprintf(what, sizeof what, "%s: controlText on a button", which);
    CheckContrast(what, t.btnText, t.btnOff, 4.5f);
    std::snprintf(what, sizeof what, "%s: the playhead on the panel", which);
    CheckContrast(what, t.playhead, t.background, 4.5f);
    std::snprintf(what, sizeof what, "%s: the accent on the panel", which);
    CheckContrast(what, t.accent, t.background, 3.0f);
    std::snprintf(what, sizeof what, "%s: MIDI green on the panel", which);
    CheckContrast(what, t.midiAccent, t.background, 3.0f);

    // A grid line the user cannot see is not a grid; one they can see from
    // across the room is a stripe. Visible, but not text.
    std::snprintf(what, sizeof what, "%s: the grid against a lane", which);
    ++g_checks;
    const float gridVsLane = ContrastRatio(t.grid, t.lane);
    if (gridVsLane < 1.15f || gridVsLane > 4.0f) {
        ++g_fails;
        std::printf("  FAIL %s: %.2f\n", what, gridVsLane);
    }
    // The alternate lane is a hint, not a second colour.
    ++g_checks;
    const float altVsLane = ContrastRatio(t.laneAlt, t.lane);
    if (altVsLane < 1.02f || altVsLane > 2.0f) {
        ++g_fails;
        std::printf("  FAIL %s: laneAlt/lane %.2f\n", which, altVsLane);
    }
    // The header strip is distinguishable from the lane it labels.
    ++g_checks;
    if (t.header == t.lane) {
        ++g_fails;
        std::printf("  FAIL %s: header == lane\n", which);
    }
}

int main() {
    // --- dark mode is the palette the app already had -----------------------
    // A regression anchor: switching modes must not redesign the dark look.
    {
        const ThemeTokens d = DeriveTokens(ThemeBase::Defaults(), ThemeMode::Dark);
        CHECK(d.background == Rgb8(30, 30, 36));
        CHECK(d.lane == Rgb8(36, 36, 42));
        CHECK(d.laneAlt == Rgb8(31, 31, 37));
        CHECK(d.ruler == Rgb8(26, 26, 31));
        CHECK(d.grid == Rgb8(20, 20, 24));
        CHECK(d.clip == Rgb8(0, 122, 255));
        CHECK(d.clipBorder == Rgb8(120, 180, 255));
        CHECK(d.wave == Rgb8(232, 240, 250));
        CHECK(d.text == Rgb8(224, 224, 224));
        CHECK(d.textDim == Rgb8(142, 142, 147));
        CHECK(d.playhead == Rgb8(240, 240, 240));
        CHECK(d.header == Rgb8(38, 38, 44));
        CHECK(d.headerHi == Rgb8(50, 50, 58));
        CHECK(d.accent == Rgb8(0, 122, 255));
        CHECK(d.audioAccent == Rgb8(0, 122, 255));
        CHECK(d.midiAccent == Rgb8(52, 199, 89));
        CHECK(d.btnOff == Rgb8(58, 58, 64));
        CHECK(d.btnBorder == Rgb8(74, 74, 80));
        CHECK(d.btnText == Rgb8(160, 160, 160));
        CHECK(d.knobBody == Rgb8(44, 44, 52));
        CHECK(d.knobOutline == Rgb8(72, 72, 82));
        CHECK(d.chrome == Rgb8(30, 30, 36));
        CHECK(d.chromeHi == Rgb8(50, 50, 58));
        CHECK(d.lcd == Rgb8(15, 16, 21));
        CHECK(d.lcdText == Rgb8(0, 240, 255));

        // The dark palette obeys the same text rule it always did.
        CheckContrast("dark: text on background", d.text, d.background, 4.5f);
        CheckContrast("dark: text on header", d.text, d.header, 4.5f);
        CheckContrast("dark: lcdText on lcd", d.lcdText, d.lcd, 4.5f);
    }

    // --- system mode, both ends of the user's Appearance --------------------
    CheckReadable(ThemeBase::Defaults(), "light panel");
    CheckReadable(DarkSystemBase(), "dark panel");

    // The mode is not allowed to be in the derivation only: the two bases must
    // actually produce different colours, or a test above is passing by
    // accident.
    {
        const ThemeTokens light = DeriveTokens(ThemeBase::Defaults(),
                                               ThemeMode::System);
        const ThemeTokens darkBase = DeriveTokens(DarkSystemBase(),
                                                  ThemeMode::System);
        CHECK(light.background != darkBase.background);
        CHECK(light.text != darkBase.text);
        CHECK(light.playhead != darkBase.playhead);
        CHECK(IsDark(darkBase.background) && !IsDark(light.background));
    }

    // --- the label rule -----------------------------------------------------
    {
        // Whichever reads better wins, and on the lamp colours it is a real
        // ratio (a recording lamp has to be readable).
        CHECK(LabelOn(Rgb8(0, 0, 0)) == Rgb8(255, 255, 255));
        CHECK(LabelOn(Rgb8(255, 255, 255)) == Rgb8(0, 0, 0));
        CheckContrast("rec lamp label", LabelOn(Rgb8(255, 59, 48)),
                      Rgb8(255, 59, 48), 4.5f);
        CheckContrast("mute lamp label", LabelOn(Rgb8(255, 204, 0)),
                      Rgb8(255, 204, 0), 4.5f);
        CheckContrast("solo lamp label", LabelOn(Rgb8(0, 176, 255)),
                      Rgb8(0, 176, 255), 4.5f);
        CheckContrast("play lamp label", LabelOn(Rgb8(52, 199, 89)),
                      Rgb8(52, 199, 89), 4.5f);
        // A clip's own label is large text, so 3:1 is the bar there.
        CheckContrast("clip label", LabelOn(Rgb8(74, 144, 217)),
                      Rgb8(74, 144, 217), 3.0f);
    }

    // --- colour maths -------------------------------------------------------
    {
        CHECK(Tint(Rgb8(100, 100, 100), 0.5f) == Rgb8(50, 50, 50));
        CHECK(Tint(Rgb8(0, 0, 0), 2.0f) == Rgb8(128, 128, 128));   // 255/2
        CHECK(Tint(Rgb8(255, 255, 255), 0.0f) == Rgb8(0, 0, 0));
        CHECK(Tint(Rgb8(10, 10, 10), 1.0f) == Rgb8(10, 10, 10));
        CHECK(Tint(Rgb8(200, 200, 200), 4.0f) == Rgb8(241, 241, 241)); // 255-55/4
        CHECK(Mix(Rgb8(0, 0, 0), Rgb8(255, 255, 255), 0.0f) == Rgb8(0, 0, 0));
        CHECK(Mix(Rgb8(0, 0, 0), Rgb8(255, 255, 255), 1.0f) == Rgb8(255, 255, 255));
        CHECK(Mix(Rgb8(0, 0, 0), Rgb8(255, 255, 255), 0.5f) == Rgb8(128, 128, 128));
        // Out-of-range and NaN inputs land somewhere legal, never wrap.
        CHECK(Mix(Rgb8(0, 0, 0), Rgb8(255, 255, 255), -1.0f) == Rgb8(0, 0, 0));
        CHECK(Mix(Rgb8(0, 0, 0), Rgb8(255, 255, 255), 2.0f) == Rgb8(255, 255, 255));
        CHECK(ContrastRatio(Rgb8(0, 0, 0), Rgb8(255, 255, 255)) > 20.9f);
        CHECK(std::fabs(ContrastRatio(Rgb8(0, 0, 0), Rgb8(0, 0, 0)) - 1.0f)
              < 1e-4f);
    }

    // --- the settings text --------------------------------------------------
    {
        CHECK(ThemeModeFromString("dark") == ThemeMode::Dark);
        CHECK(ThemeModeFromString("system") == ThemeMode::System);
        // A hand-edited or future settings file can never leave the app dark.
        CHECK(ThemeModeFromString("") == ThemeMode::System);
        CHECK(ThemeModeFromString("Dark") == ThemeMode::System);
        CHECK(ThemeModeFromString(nullptr) == ThemeMode::System);
        CHECK(ThemeModeFromString("midnight") == ThemeMode::System);
        CHECK(std::string(ThemeModeToString(ThemeMode::Dark)) == "dark");
        CHECK(std::string(ThemeModeToString(ThemeMode::System)) == "system");
    }

    std::printf("\n%d checks, %d failures\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
