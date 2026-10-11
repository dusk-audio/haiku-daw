// TpdfDither — the 16-bit triangular-PDF dither the WAV writer has always
// applied, lifted out so a FLAC file at the same depth is dithered by the same
// generator (and a future AIFF writer inherits it).
//
// It is a header-only kit-free struct on purpose: the algorithm and the SEED
// are part of the output. Changing either changes every dithered bounce byte
// for byte, and the exporter's tests compare rendered files exactly -- so this
// is a move, not a rewrite.
#pragma once

#include <cstdint>

namespace daw {

struct TpdfDither {
    // The seed the WAV writer has used since the export path was written.
    uint32_t state = 0x1234567u;

    // One dither value in (-1, 1) LSB: the difference of two uniforms, i.e. a
    // triangular distribution of width 2 LSB. Add it to the sample scaled to
    // LSB units, then round.
    double Next() {
        // xorshift32, the same three shifts the writer has always used.
        auto next = [this]() {
            state ^= state << 13;
            state ^= state >> 17;
            state ^= state << 5;
            return state;
        };
        const double r1 = (next() >> 8) * (1.0 / 16777216.0);
        const double r2 = (next() >> 8) * (1.0 / 16777216.0);
        return r1 - r2;
    }
};

} // namespace daw
