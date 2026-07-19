#include "Sf2Reader.h"

#include <algorithm>
#include <fstream>

namespace daw {

const Sf2Generator* Sf2Zone::Find(uint16_t oper) const {
    for (const Sf2Generator& g : gens)
        if (g.oper == oper) return &g;
    return nullptr;
}

namespace {

// Fixed record sizes from the SF2 2.04 spec.
constexpr int kPhdrSize = 38;
constexpr int kBagSize  = 4;
constexpr int kGenSize  = 4;
constexpr int kInstSize = 22;
constexpr int kShdrSize = 46;

// A cursor over an in-memory byte span. All SF2 multibyte fields are LE.
struct Cursor {
    const uint8_t* p    = nullptr;
    size_t         size = 0;
    size_t         pos  = 0;

    bool CanRead(size_t n) const { return pos + n <= size; }

    uint8_t  U8()  { return p[pos++]; }
    int8_t   S8()  { return (int8_t)p[pos++]; }
    uint16_t U16() {
        const uint16_t v = (uint16_t)(p[pos] | (p[pos + 1] << 8));
        pos += 2;
        return v;
    }
    uint32_t U32() {
        const uint32_t v = (uint32_t)p[pos] | ((uint32_t)p[pos + 1] << 8)
                         | ((uint32_t)p[pos + 2] << 16) | ((uint32_t)p[pos + 3] << 24);
        pos += 4;
        return v;
    }
    // SF2 name fields are NUL-padded ASCII up to n bytes.
    std::string FixedStr(int n) {
        const char* s = (const char*)(p + pos);
        int len = 0;
        while (len < n && s[len] != '\0') len++;
        std::string out(s, (size_t)len);
        pos += (size_t)n;
        return out;
    }
};

// One sub-chunk of a LIST: 4-char id + raw bytes.
struct SubChunk {
    char           id[5] = { 0, 0, 0, 0, 0 };
    const uint8_t* data  = nullptr;
    uint32_t       size  = 0;
};

bool IdEquals(const char* a, const char* b) {
    return a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3];
}

int64_t Clamp64(int64_t v, int64_t lo, int64_t hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

// Whole-parse resource budget.
//
// A PER-CALL cap does not bound a file: BuildZones runs once per preset and
// once per instrument, so a bank with 40 of each multiplies any per-call limit
// by 80. And an EMPTY zone still costs a vector, so counting only generators
// misses the 65k-empty-zones shape entirely. Both counters are therefore shared
// across the whole parse, and both include the degenerate cases.
struct ParseBudget {
    size_t zones    = 0;
    size_t gens     = 0;
    bool   exceeded = false;

    static constexpr size_t kMaxZones = 1u << 18;   // 262144 zones
    static constexpr size_t kMaxGens  = 1u << 21;   // 2 M gens, ~8 MB

    // Returns false once either limit is blown; the caller stops and the parse
    // is reported as failed rather than returning partial data.
    bool Take(size_t nZones, size_t nGens) {
        if (exceeded) return false;
        if (zones + nZones > kMaxZones || gens + nGens > kMaxGens) {
            exceeded = true;
            return false;
        }
        zones += nZones;
        gens  += nGens;
        return true;
    }
};

// Resolve one preset's/instrument's zone list from the bag + gen tables.
// bagStart/bagEnd index the bag table; each bag entry's genNdx points into the
// gen table, and the NEXT bag's genNdx bounds this zone.
std::vector<Sf2Zone> BuildZones(int bagStart, int bagEnd,
                                const std::vector<uint16_t>& bagGenNdx,
                                const std::vector<Sf2Generator>& gens,
                                ParseBudget& budget) {
    std::vector<Sf2Zone> zones;
    const int maxBag = (int)bagGenNdx.size();
    for (int b = bagStart; b < bagEnd; b++) {
        // A zone needs bag[b] and bag[b+1]. A malformed bank can carry a header
        // bag index past the bag table — bail rather than read out of bounds.
        if (b < 0 || b + 1 >= maxBag) break;
        const int nGens    = (int)gens.size();
        const int genStart = std::clamp((int)bagGenNdx[(size_t)b], 0, nGens);
        const int genEnd   = std::clamp((int)bagGenNdx[(size_t)b + 1], 0, nGens);
        const size_t want  = (genEnd > genStart) ? (size_t)(genEnd - genStart) : 0;
        if (!budget.Take(1, want)) break;     // whole-parse budget blown
        Sf2Zone z;
        for (int g = genStart; g < genEnd; g++)
            z.gens.push_back(gens[(size_t)g]);
        zones.push_back(std::move(z));
    }
    return zones;
}

} // namespace

Sf2File ReadSf2(const std::string& path) {
    Sf2File out;

    std::ifstream in(path, std::ios::binary);
    if (!in) {
        out.error = "could not open '" + path + "'";
        return out;
    }
    in.seekg(0, std::ios::end);
    const int64_t fileLen = (int64_t)in.tellg();
    in.seekg(0, std::ios::beg);

    auto readBytes = [&](void* dst, int64_t n) -> bool {
        in.read((char*)dst, (std::streamsize)n);
        return in.gcount() == (std::streamsize)n;
    };
    auto readU32 = [&]() -> uint32_t {
        uint8_t b[4];
        if (!readBytes(b, 4)) return 0;
        return (uint32_t)b[0] | ((uint32_t)b[1] << 8)
             | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
    };

    // RIFF header: 'RIFF' <u32 size> 'sfbk'.
    char riff[4], form[4];
    if (!readBytes(riff, 4) || !IdEquals(riff, "RIFF")) {
        out.error = "not a RIFF file";
        return out;
    }
    readU32();   // overall size (ignored)
    if (!readBytes(form, 4) || !IdEquals(form, "sfbk")) {
        out.error = "not an SF2 (sfbk) file";
        return out;
    }

    std::vector<uint8_t> pdtaBytes;

    // Walk the top-level LIST chunks.
    while (in && in.tellg() >= 0 && (int64_t)in.tellg() + 8 <= fileLen) {
        char ck[4];
        if (!readBytes(ck, 4)) break;
        const uint32_t ckSize = readU32();
        const int64_t  ckBody = (int64_t)in.tellg();

        if (IdEquals(ck, "LIST")) {
            char listType[4];
            if (!readBytes(listType, 4)) break;

            if (IdEquals(listType, "sdta")) {
                // Record where the PCM lives; skip the (large) bytes.
                const int64_t sdtaEnd = ckBody + (int64_t)ckSize;
                while ((int64_t)in.tellg() + 8 <= sdtaEnd) {
                    char sid[4];
                    if (!readBytes(sid, 4)) break;
                    const uint32_t sSize = readU32();
                    const int64_t  sBody = (int64_t)in.tellg();
                    if (IdEquals(sid, "smpl")) {
                        out.smplOffset = sBody;
                        // Clamp to the file's real remainder: smplSize is the
                        // bound sample extraction validates shdr ranges
                        // against, so it must never exceed what the file can
                        // actually deliver.
                        out.smplSize = Clamp64((int64_t)sSize, 0, fileLen - sBody);
                    } else if (IdEquals(sid, "sm24")) {
                        out.sm24Offset = sBody;
                        out.sm24Size   = Clamp64((int64_t)sSize, 0, fileLen - sBody);
                    }
                    in.seekg(sBody + (int64_t)sSize + ((sSize & 1) ? 1 : 0),
                             std::ios::beg);
                }
            } else if (IdEquals(listType, "pdta")) {
                // Slurp the whole pdta body — metadata, small even for a 140 MB
                // GM bank. Clamp to the bytes actually left in the file: a
                // crafted ckSize (~2 GB) must not size an allocation the read
                // can never fill.
                const int64_t here = (int64_t)in.tellg();
                // pdta is metadata only: even a 140 MB GM bank keeps it in the
                // low megabytes. Cap it before the resize so a file declaring a
                // multi-GB pdta cannot size the allocation, however large the
                // file itself is.
                constexpr int64_t kMaxPdtaBytes = 64ll * 1024 * 1024;
                const int64_t bodyLen =
                    Clamp64((int64_t)ckSize - 4, 0,
                            std::min<int64_t>(fileLen - here, kMaxPdtaBytes));
                if (bodyLen > 0) {
                    pdtaBytes.resize((size_t)bodyLen);
                    in.read((char*)pdtaBytes.data(), (std::streamsize)bodyLen);
                    // Truncated file: shrink to what actually arrived so the
                    // sub-chunk parser never walks uninitialised tail bytes.
                    const int64_t got = (int64_t)in.gcount();
                    if (got < bodyLen)
                        pdtaBytes.resize((size_t)std::max<int64_t>(0, got));
                }
            }
        }

        in.clear();   // a short read above may have set failbit/eofbit
        in.seekg(ckBody + (int64_t)ckSize + ((ckSize & 1) ? 1 : 0), std::ios::beg);
    }

    if (pdtaBytes.empty()) {
        out.error = "SF2 has no pdta chunk";
        return out;
    }

    // Split pdta into its named sub-chunks.
    SubChunk phdr, pbag, pgen, inst, ibag, igen, shdr;
    {
        Cursor c{ pdtaBytes.data(), pdtaBytes.size(), 0 };
        while (c.CanRead(8)) {
            SubChunk sc;
            sc.id[0] = (char)c.U8(); sc.id[1] = (char)c.U8();
            sc.id[2] = (char)c.U8(); sc.id[3] = (char)c.U8();
            sc.size  = c.U32();
            sc.data  = c.p + c.pos;
            if (!c.CanRead(sc.size)) break;
            c.pos += sc.size + ((sc.size & 1) ? 1 : 0);

            if      (IdEquals(sc.id, "phdr")) phdr = sc;
            else if (IdEquals(sc.id, "pbag")) pbag = sc;
            else if (IdEquals(sc.id, "pgen")) pgen = sc;
            else if (IdEquals(sc.id, "inst")) inst = sc;
            else if (IdEquals(sc.id, "ibag")) ibag = sc;
            else if (IdEquals(sc.id, "igen")) igen = sc;
            else if (IdEquals(sc.id, "shdr")) shdr = sc;
        }
    }

    if (phdr.data == nullptr || inst.data == nullptr || shdr.data == nullptr) {
        out.error = "SF2 pdta missing required sub-chunks";
        return out;
    }

    // Samples (shdr).
    {
        const int n = (int)(shdr.size / kShdrSize);
        Cursor c{ shdr.data, shdr.size, 0 };
        for (int i = 0; i < n; i++) {
            Sf2Sample s;
            s.name            = c.FixedStr(20);
            s.start           = c.U32();
            s.end             = c.U32();
            s.startLoop       = c.U32();
            s.endLoop         = c.U32();
            s.sampleRate      = c.U32();
            s.originalPitch   = c.U8();
            s.pitchCorrection = c.S8();
            s.sampleLink      = c.U16();
            s.sampleType      = c.U16();
            // The last record is the "EOS" terminal sentinel — drop it.
            if (i == n - 1 && s.name.rfind("EOS", 0) == 0) break;
            out.samples.push_back(std::move(s));
        }
    }

    ParseBudget budget;

    // Instrument generator zones.
    std::vector<uint16_t>     ibagGenNdx;
    std::vector<Sf2Generator> igens;
    {
        const int n = (int)(igen.size / kGenSize);
        Cursor c{ igen.data, igen.size, 0 };
        for (int i = 0; i < n; i++) {
            Sf2Generator g;
            g.oper   = c.U16();
            g.amount = c.U16();
            igens.push_back(g);
        }
        const int nb = (int)(ibag.size / kBagSize);
        Cursor cb{ ibag.data, ibag.size, 0 };
        for (int i = 0; i < nb; i++) {
            ibagGenNdx.push_back(cb.U16());   // genNdx
            cb.U16();                         // modNdx (ignored)
        }
    }
    {
        const int n = (int)(inst.size / kInstSize);
        Cursor c{ inst.data, inst.size, 0 };
        std::vector<std::string> names;
        std::vector<int>         bagNdx;
        for (int i = 0; i < n; i++) {
            names.push_back(c.FixedStr(20));
            bagNdx.push_back((int)c.U16());
        }
        // n includes the terminal "EOI"; real instruments are [0, n-1).
        for (int i = 0; i + 1 < n; i++) {
            Sf2Instrument ins;
            ins.name  = names[(size_t)i];
            ins.zones = BuildZones(bagNdx[(size_t)i], bagNdx[(size_t)i + 1],
                                   ibagGenNdx, igens, budget);
            out.instruments.push_back(std::move(ins));
        }
    }

    // Preset generator zones.
    std::vector<uint16_t>     pbagGenNdx;
    std::vector<Sf2Generator> pgens;
    {
        const int n = (int)(pgen.size / kGenSize);
        Cursor c{ pgen.data, pgen.size, 0 };
        for (int i = 0; i < n; i++) {
            Sf2Generator g;
            g.oper   = c.U16();
            g.amount = c.U16();
            pgens.push_back(g);
        }
        const int nb = (int)(pbag.size / kBagSize);
        Cursor cb{ pbag.data, pbag.size, 0 };
        for (int i = 0; i < nb; i++) {
            pbagGenNdx.push_back(cb.U16());
            cb.U16();
        }
    }
    {
        const int n = (int)(phdr.size / kPhdrSize);
        Cursor c{ phdr.data, phdr.size, 0 };
        std::vector<std::string> names;
        std::vector<uint16_t>    progs, banks;
        std::vector<int>         bagNdx;
        for (int i = 0; i < n; i++) {
            names.push_back(c.FixedStr(20));
            progs.push_back(c.U16());
            banks.push_back(c.U16());
            bagNdx.push_back((int)c.U16());
            c.U32(); c.U32(); c.U32();   // library / genre / morphology
        }
        for (int i = 0; i + 1 < n; i++) {   // skip the terminal "EOP"
            Sf2Preset pr;
            pr.name   = names[(size_t)i];
            pr.preset = progs[(size_t)i];
            pr.bank   = banks[(size_t)i];
            pr.zones  = BuildZones(bagNdx[(size_t)i], bagNdx[(size_t)i + 1],
                                   pbagGenNdx, pgens, budget);
            out.presets.push_back(std::move(pr));
        }
    }

    if (budget.exceeded) {
        // Partial zone data is worse than none: a caller seeing ok=true would
        // treat a truncated instrument map as the real thing. Report failure.
        out.presets.clear();
        out.instruments.clear();
        out.error = "SF2 zone/generator tables exceed the parser's limits "
                    "(malformed or hostile bank)";
        return out;
    }

    out.ok = true;
    return out;
}

} // namespace daw
