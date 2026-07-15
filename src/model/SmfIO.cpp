#include "SmfIO.h"

#include <algorithm>
#include <fstream>
#include <map>

namespace daw {

namespace {

// ---- Reading helpers over an in-memory byte buffer + cursor ----------------

struct Reader {
    const uint8_t* p;
    const uint8_t* end;
    bool ok = true;

    bool has(size_t n) const { return (size_t)(end - p) >= n; }
    uint8_t  u8()  { if (!has(1)) { ok = false; return 0; } return *p++; }
    uint16_t u16() { uint16_t h = u8(); return (uint16_t)((h << 8) | u8()); }
    uint32_t u32() { uint32_t v = u16(); return (v << 16) | u16(); }

    // Variable-length quantity (7 bits/byte, MSB = continue).
    uint32_t varlen() {
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i) {
            const uint8_t b = u8();
            v = (v << 7) | (b & 0x7F);
            if (!(b & 0x80)) break;
        }
        return v;
    }
};

// ---- Writing helpers -------------------------------------------------------

void PutU16(std::vector<uint8_t>& b, uint16_t v) {
    b.push_back((uint8_t)(v >> 8)); b.push_back((uint8_t)v);
}
void PutU32(std::vector<uint8_t>& b, uint32_t v) {
    b.push_back((uint8_t)(v >> 24)); b.push_back((uint8_t)(v >> 16));
    b.push_back((uint8_t)(v >> 8));  b.push_back((uint8_t)v);
}
void PutVarlen(std::vector<uint8_t>& b, uint32_t v) {
    uint8_t stack[5]; int n = 0;
    stack[n++] = v & 0x7F;
    while ((v >>= 7)) stack[n++] = (uint8_t)((v & 0x7F) | 0x80);
    while (n) b.push_back(stack[--n]);
}

// A raw timed event, used to sort a track's note on/offs before delta-encoding.
struct Ev {
    uint32_t tick;
    int      order;   // tie-break: offs (0) before ons (1) at the same tick
    uint8_t  status, d1, d2;
};

} // namespace

bool ReadSmf(const std::string& path, SmfData& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::vector<uint8_t> buf((std::istreambuf_iterator<char>(f)),
                             std::istreambuf_iterator<char>());
    Reader r{ buf.data(), buf.data() + buf.size() };

    // Header: "MThd" len(6) format ntrks division.
    if (r.u8() != 'M' || r.u8() != 'T' || r.u8() != 'h' || r.u8() != 'd')
        return false;
    const uint32_t hlen = r.u32();
    r.u16();                         // format (0/1/2) — we treat all the same
    const uint16_t ntrks = r.u16();
    const uint16_t division = r.u16();
    if (!r.ok || (division & 0x8000)) return false;   // reject SMPTE division
    // Skip any extra header bytes (hlen should be 6).
    for (uint32_t i = 6; i < hlen && r.ok; ++i) r.u8();

    out = SmfData{};
    out.division = division ? division : 480;

    bool gotTempo = false;
    for (uint16_t t = 0; t < ntrks && r.ok; ++t) {
        // Track chunk: "MTrk" len ...events.
        if (r.u8() != 'M' || r.u8() != 'T' || r.u8() != 'r' || r.u8() != 'k')
            break;
        const uint32_t tlen = r.u32();
        const uint8_t* trackEnd = r.p + tlen;
        if (trackEnd > r.end) trackEnd = r.end;

        SmfTrack track;
        // Notes still waiting for their note-off, keyed by (channel<<8)|key.
        std::map<int, std::pair<uint32_t, int>> pending;   // -> (startTick, vel)
        uint32_t now = 0;
        uint8_t  status = 0;   // running status

        while (r.p < trackEnd && r.ok) {
            now += r.varlen();
            uint8_t b = r.u8();
            if (b & 0x80) { status = b; }         // new status byte
            else          { r.p--; }              // running status: reuse
            const uint8_t hi = status & 0xF0;
            const uint8_t ch = status & 0x0F;

            if (status == 0xFF) {                 // meta event
                const uint8_t type = r.u8();
                const uint32_t len = r.varlen();
                const uint8_t* data = r.p;
                if (type == 0x51 && len == 3 && r.has(3)) {   // set tempo
                    const uint32_t usPerQn =
                        ((uint32_t)data[0] << 16) | ((uint32_t)data[1] << 8) | data[2];
                    if (!gotTempo && usPerQn > 0) {
                        out.tempoBpm = 60000000.0 / (double)usPerQn;
                        gotTempo = true;
                    }
                } else if (type == 0x03 && len > 0 && r.has(len)) {   // track name
                    track.name.assign((const char*)data, (size_t)len);
                }
                r.p += len;
            } else if (status == 0xF0 || status == 0xF7) {   // sysex: skip
                const uint32_t len = r.varlen();
                r.p += len;
            } else if (hi == 0x90 || hi == 0x80) {           // note on / off
                const uint8_t key = r.u8();
                const uint8_t vel = r.u8();
                const int id = (ch << 8) | key;
                const bool on = (hi == 0x90) && vel > 0;
                if (on) {
                    pending[id] = { now, vel };
                } else {
                    auto it = pending.find(id);
                    if (it != pending.end()) {
                        SmfNote nn;
                        nn.pitch      = key;
                        nn.velocity   = it->second.second;
                        nn.startTick  = it->second.first;
                        nn.lengthTick = now - it->second.first;
                        track.notes.push_back(nn);
                        pending.erase(it);
                    }
                }
            } else if (hi == 0xC0 || hi == 0xD0) {           // 1 data byte
                r.u8();
            } else if (hi == 0xA0 || hi == 0xB0 || hi == 0xE0) {   // 2 data bytes
                r.u8(); r.u8();
            } else {
                break;   // unknown status: bail on this track
            }
        }
        // Any notes without a matching off end at the last event.
        for (auto& kv : pending) {
            SmfNote nn;
            nn.pitch = kv.first & 0xFF;
            nn.velocity = kv.second.second;
            nn.startTick = kv.second.first;
            nn.lengthTick = now > kv.second.first ? now - kv.second.first : 0;
            track.notes.push_back(nn);
        }
        std::sort(track.notes.begin(), track.notes.end(),
                  [](const SmfNote& a, const SmfNote& b) {
                      return a.startTick < b.startTick;
                  });
        r.p = trackEnd;                 // resync to the declared track end
        out.tracks.push_back(std::move(track));
    }
    return !out.tracks.empty();
}

bool WriteSmf(const std::string& path, const SmfData& in) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;

    std::vector<uint8_t> file;
    // Header.
    file.push_back('M'); file.push_back('T'); file.push_back('h'); file.push_back('d');
    PutU32(file, 6);
    PutU16(file, 1);                                   // format 1
    PutU16(file, (uint16_t)std::max<size_t>(1, in.tracks.size()));
    PutU16(file, in.division ? in.division : 480);

    for (size_t ti = 0; ti < in.tracks.size(); ++ti) {
        const SmfTrack& tr = in.tracks[ti];
        std::vector<Ev> evs;
        for (const SmfNote& n : tr.notes) {
            int v = n.velocity; if (v < 1) v = 1; if (v > 127) v = 127;
            evs.push_back({ n.startTick, 1, 0x90, (uint8_t)(n.pitch & 0x7F),
                            (uint8_t)v });
            evs.push_back({ n.startTick + n.lengthTick, 0, 0x80,
                            (uint8_t)(n.pitch & 0x7F), 0x40 });
        }
        std::stable_sort(evs.begin(), evs.end(), [](const Ev& a, const Ev& b) {
            return a.tick != b.tick ? a.tick < b.tick : a.order < b.order;
        });

        std::vector<uint8_t> body;
        // Track name (meta 0x03).
        if (!tr.name.empty()) {
            PutVarlen(body, 0);
            body.push_back(0xFF); body.push_back(0x03);
            PutVarlen(body, (uint32_t)tr.name.size());
            body.insert(body.end(), tr.name.begin(), tr.name.end());
        }
        // Tempo (meta 0x51) at the head of the first track.
        if (ti == 0) {
            const double bpm = in.tempoBpm > 0 ? in.tempoBpm : 120.0;
            const uint32_t usPerQn = (uint32_t)(60000000.0 / bpm + 0.5);
            PutVarlen(body, 0);
            body.push_back(0xFF); body.push_back(0x51); body.push_back(0x03);
            body.push_back((uint8_t)(usPerQn >> 16));
            body.push_back((uint8_t)(usPerQn >> 8));
            body.push_back((uint8_t)usPerQn);
        }
        // Note events, delta-encoded (explicit status each time; no running
        // status on write, for simplicity).
        uint32_t prev = 0;
        for (const Ev& e : evs) {
            PutVarlen(body, e.tick - prev);
            prev = e.tick;
            body.push_back(e.status); body.push_back(e.d1); body.push_back(e.d2);
        }
        // End of track.
        PutVarlen(body, 0);
        body.push_back(0xFF); body.push_back(0x2F); body.push_back(0x00);

        file.push_back('M'); file.push_back('T'); file.push_back('r'); file.push_back('k');
        PutU32(file, (uint32_t)body.size());
        file.insert(file.end(), body.begin(), body.end());
    }

    f.write((const char*)file.data(), (std::streamsize)file.size());
    return (bool)f;
}

} // namespace daw
