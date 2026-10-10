// DawIcons — the app's small vector glyphs, drawn with the drawing API
// (plan M1.6: "vector toolbar and tool-palette icons ... drawn with BShape").
//
// Drawn rather than shipped as bitmaps so they stay sharp at any size and pick
// up the theme's colours: a glyph takes the colour it is given, so a lit tool
// and a dim one are the same code. Each is authored inside a unit square and
// mapped onto the rect it is handed.
#pragma once

#include "../Theme.h"

#include <Shape.h>
#include <View.h>

#include <algorithm>

namespace daw {

namespace icons {

// Map a unit-square point (0..1) onto `r`, keeping the glyph square.
inline BPoint P(const BRect& r, float x, float y) {
    const float s = std::min(r.Width(), r.Height());
    const float ox = r.left + (r.Width() - s) * 0.5f;
    const float oy = r.top + (r.Height() - s) * 0.5f;
    return BPoint(ox + x * s, oy + y * s);
}

// The tool palette of the MIDI editor, in kToolNames order: Pointer, Pencil,
// Brush, Eraser, Scissors, Glue, Velocity.
inline void DrawTool(BView* v, BRect r, int tool, rgb_color color) {
    if (!r.IsValid()) return;
    v->SetHighColor(color);
    const float pen = std::max(Themed(1.2f), 1.0f);
    v->SetPenSize(pen);
    switch (tool) {
        case 0: {   // pointer: a filled arrow
            BPoint p[3] = { P(r, 0.30f, 0.18f), P(r, 0.30f, 0.74f),
                            P(r, 0.72f, 0.52f) };
            v->FillPolygon(p, 3);
            break;
        }
        case 1: {   // pencil: a shaft with a nib
            v->StrokeLine(P(r, 0.30f, 0.72f), P(r, 0.68f, 0.26f));
            v->StrokeLine(P(r, 0.36f, 0.78f), P(r, 0.74f, 0.32f));
            BPoint nib[3] = { P(r, 0.24f, 0.80f), P(r, 0.36f, 0.78f),
                              P(r, 0.30f, 0.66f) };
            v->FillPolygon(nib, 3);
            break;
        }
        case 2: {   // brush: a wider stroke with a splayed tip
            v->StrokeLine(P(r, 0.42f, 0.74f), P(r, 0.70f, 0.26f));
            v->StrokeLine(P(r, 0.56f, 0.78f), P(r, 0.80f, 0.34f));
            v->FillEllipse(P(r, 0.34f, 0.78f), r.Width() * 0.12f,
                           r.Width() * 0.12f);
            break;
        }
        case 3: {   // eraser: a slanted block
            BPoint p[4] = { P(r, 0.30f, 0.58f), P(r, 0.58f, 0.24f),
                            P(r, 0.78f, 0.44f), P(r, 0.50f, 0.78f) };
            v->FillPolygon(p, 4);
            break;
        }
        case 4: {   // scissors: two blades crossing
            v->StrokeLine(P(r, 0.28f, 0.24f), P(r, 0.68f, 0.68f));
            v->StrokeLine(P(r, 0.72f, 0.24f), P(r, 0.32f, 0.68f));
            v->StrokeEllipse(P(r, 0.28f, 0.78f), r.Width() * 0.11f,
                             r.Width() * 0.11f);
            v->StrokeEllipse(P(r, 0.72f, 0.78f), r.Width() * 0.11f,
                             r.Width() * 0.11f);
            break;
        }
        case 5: {   // glue: two blocks meeting
            v->FillRect(BRect(P(r, 0.24f, 0.42f), P(r, 0.46f, 0.62f)));
            v->FillRect(BRect(P(r, 0.54f, 0.42f), P(r, 0.76f, 0.62f)));
            v->StrokeLine(P(r, 0.34f, 0.26f), P(r, 0.66f, 0.26f));
            v->StrokeLine(P(r, 0.34f, 0.78f), P(r, 0.66f, 0.78f));
            break;
        }
        default: {  // 6 velocity: three rising bars
            const float xs[3] = { 0.26f, 0.46f, 0.66f };
            const float hs[3] = { 0.28f, 0.46f, 0.64f };
            for (int i = 0; i < 3; i++)
                v->FillRect(BRect(P(r, xs[i], 0.80f - hs[i]),
                                  P(r, xs[i] + 0.14f, 0.80f)));
            break;
        }
    }
    v->SetPenSize(1.0f);
}

// Transport glyphs (the strip draws its own, at its own size; these are for
// menus and dialogs that need the same shapes small).
inline void DrawTransport(BView* v, BRect r, int which, rgb_color color) {
    if (!r.IsValid()) return;
    v->SetHighColor(color);
    switch (which) {
        case 0: {   // play
            BPoint p[3] = { P(r, 0.32f, 0.22f), P(r, 0.32f, 0.78f),
                            P(r, 0.76f, 0.50f) };
            v->FillPolygon(p, 3);
            break;
        }
        case 1:     // stop
            v->FillRect(BRect(P(r, 0.28f, 0.28f), P(r, 0.72f, 0.72f)));
            break;
        default:    // record
            v->FillEllipse(P(r, 0.50f, 0.50f), r.Width() * 0.22f,
                           r.Width() * 0.22f);
            break;
    }
}

} // namespace icons

} // namespace daw
