#include "SfzParser.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>

namespace daw {
namespace {

// ---------------------------------------------------------------- numbers --

// Locale-independent float parse. std::strtod follows the C locale's decimal
// separator, so under a comma locale "pan=37.795" would stop at the '.' and
// yield 37. SFZ always uses '.', and a DAW's locale is not its business, so
// parse the digits directly. Accepts leading sign, digits, one '.', and an
// optional exponent.
float ParseFloat(const std::string& s, float fallback) {
    const char* p = s.c_str();
    while (*p == ' ' || *p == '\t') p++;

    bool neg = false;
    if (*p == '+' || *p == '-') { neg = (*p == '-'); p++; }

    bool   any = false;
    double v   = 0.0;
    while (std::isdigit((unsigned char)*p)) { v = v * 10.0 + (*p - '0'); p++; any = true; }
    if (*p == '.') {
        p++;
        double scale = 0.1;
        while (std::isdigit((unsigned char)*p)) {
            v += (*p - '0') * scale;
            scale *= 0.1;
            p++;
            any = true;
        }
    }
    if (!any) return fallback;

    if (*p == 'e' || *p == 'E') {
        const char* save = p;
        p++;
        bool eneg = false;
        if (*p == '+' || *p == '-') { eneg = (*p == '-'); p++; }
        if (std::isdigit((unsigned char)*p)) {
            // Saturate rather than overflow: "1e999999999999" would otherwise
            // wrap `e` (signed overflow, undefined behaviour). Any exponent past
            // ~330 already takes the double to 0 or inf, so clamping is exact
            // for every value that matters.
            int e = 0;
            while (std::isdigit((unsigned char)*p)) {
                if (e < 100000) e = e * 10 + (*p - '0');
                p++;
            }
            v *= std::pow(10.0, eneg ? -e : e);
        } else {
            p = save;   // a stray 'e' is not an exponent
        }
    }
    return (float)(neg ? -v : v);
}

int64_t ParseInt(const std::string& s, int64_t fallback) {
    const char* p = s.c_str();
    while (*p == ' ' || *p == '\t') p++;
    bool neg = false;
    if (*p == '+' || *p == '-') { neg = (*p == '-'); p++; }
    if (!std::isdigit((unsigned char)*p)) return fallback;
    // Saturate instead of overflowing int64 (undefined behaviour) on a value
    // like "99999999999999999999999". Every consumer clamps into a real range
    // downstream, so the saturated value behaves identically to the true one.
    constexpr int64_t kMax = 9223372036854775807ll;
    int64_t v = 0;
    bool sat = false;
    while (std::isdigit((unsigned char)*p)) {
        const int d = *p - '0';
        if (!sat) {
            if (v > (kMax - d) / 10) sat = true;
            else                     v = v * 10 + d;
        }
        p++;
    }
    if (sat) v = kMax;
    return neg ? -v : v;
}

// Key opcodes accept either a MIDI number or a note name (c4, f#3, Bb-1),
// where c4 == 60. Returns `fallback` if neither parses.
int ParseKey(const std::string& s, int fallback) {
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) i++;
    if (i >= s.size()) return fallback;

    if (std::isdigit((unsigned char)s[i]) || s[i] == '-' || s[i] == '+')
        return (int)ParseInt(s, fallback);

    static const int kSemis[7] = { 9, 11, 0, 2, 4, 5, 7 };   // a b c d e f g
    const char c = (char)std::tolower((unsigned char)s[i]);
    if (c < 'a' || c > 'g') return fallback;
    int semi = kSemis[c - 'a'];
    i++;

    while (i < s.size() && (s[i] == '#' || s[i] == 'b' || s[i] == 'B'
                            || s[i] == '-' || s[i] == '+'
                            || std::isdigit((unsigned char)s[i]))) {
        if (s[i] == '#' || s[i] == '+') { semi++; i++; continue; }
        if (s[i] == 'b' || s[i] == 'B') { semi--; i++; continue; }
        break;   // start of the octave number
    }
    if (i >= s.size()) return fallback;
    const int octave = (int)ParseInt(s.substr(i), -99);
    if (octave == -99) return fallback;
    return (octave + 1) * 12 + semi;   // c4 = 60
}

// ------------------------------------------------------------------ paths --

std::string DirOf(const std::string& path) {
    const size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? std::string(".") : path.substr(0, slash);
}

// Join a base directory with a possibly-relative SFZ path. Sample paths in
// commercially authored files routinely use Windows backslashes, so normalise
// them — otherwise every such library silently loads zero samples.
std::string ResolvePath(const std::string& baseDir, const std::string& rel) {
    std::string r = rel;
    for (char& c : r) if (c == '\\') c = '/';
    while (!r.empty() && (r.front() == ' ' || r.front() == '\t')) r.erase(r.begin());
    while (!r.empty() && (r.back() == ' ' || r.back() == '\t')) r.pop_back();
    if (r.empty()) return r;
    if (r[0] == '/') return r;                       // already absolute
    // Windows drive-absolute ("C:/Samples/x.wav") after the backslash
    // normalisation above. Joining baseDir onto it would produce nonsense like
    // "/home/me/kit/C:/Samples/x.wav"; leave it alone so the path is reported
    // as-is when it inevitably does not resolve on this host.
    if (r.size() >= 3 && r[1] == ':' && r[2] == '/'
        && std::isalpha((unsigned char)r[0]))
        return r;
    if (baseDir.empty() || baseDir == ".") return r;
    return baseDir + "/" + r;
}

// ----------------------------------------------------------------- lexing --

using OpcodeMap = std::map<std::string, std::string>;

// One lexed element of an SFZ file: either a <header> or an opcode=value.
struct Token {
    bool        isHeader = false;
    std::string name;    // header name, or opcode name
    std::string value;   // opcode value (empty for headers)
};

bool IsOpcodeChar(char c) {
    return std::isalnum((unsigned char)c) || c == '_' || c == '$';
}

// Strip // line comments and /* */ block comments. `inBlock` carries block
// state across lines.
std::string StripComments(const std::string& line, bool* inBlock) {
    std::string out;
    size_t i = 0;
    while (i < line.size()) {
        if (*inBlock) {
            if (i + 1 < line.size() && line[i] == '*' && line[i + 1] == '/') {
                *inBlock = false;
                i += 2;
            } else {
                i++;
            }
            continue;
        }
        if (i + 1 < line.size() && line[i] == '/' && line[i + 1] == '/')
            break;                                   // rest of line is comment
        if (i + 1 < line.size() && line[i] == '/' && line[i + 1] == '*') {
            *inBlock = true;
            i += 2;
            continue;
        }
        out.push_back(line[i]);
        i++;
    }
    return out;
}

// Lex one comment-stripped line into headers and opcodes.
//
// The awkward part of SFZ: an opcode value may contain spaces (sample names
// nearly always do — "sample=Bd Brush 1.wav lokey=36"), and there are no
// quotes. The rule is that a value runs up to the start of the NEXT opcode
// name, i.e. back off from the following '=' over its identifier and the
// whitespace before it. So find all '=' positions first, then slice between.
void LexLine(const std::string& line, std::vector<Token>* out) {
    // Headers and opcodes must be emitted in SOURCE ORDER. Collecting all the
    // headers first and the opcodes afterwards misplaces every opcode that
    // precedes a header on the same line: in
    //     <group> volume=-6 <region> sample=x.wav
    // the volume belongs to the group, but a header-first pass opens the region
    // before the volume is applied and it lands on the region instead.
    struct Item {
        size_t pos;        // where it starts, for ordering
        bool   isHeader;
        size_t a, b;       // header: '<' and '>'; opcode: name start and '='
    };
    std::vector<Item> items;

    // Headers, recorded with their spans so the opcode pass can ignore any '='
    // that falls inside one.
    std::vector<std::pair<size_t, size_t>> hdrs;
    for (size_t i = 0; ; ) {
        const size_t lt = line.find('<', i);
        if (lt == std::string::npos) break;
        const size_t gt = line.find('>', lt);
        if (gt == std::string::npos) break;
        hdrs.push_back({ lt, gt });
        items.push_back({ lt, true, lt, gt });
        i = gt + 1;
    }
    auto inHeader = [&](size_t p) {
        for (const auto& h : hdrs)
            if (p >= h.first && p <= h.second) return true;
        return false;
    };

    // Every '=' that terminates an identifier and is not inside a header.
    for (size_t i = 0; i < line.size(); i++) {
        if (line[i] != '=' || inHeader(i)) continue;
        size_t j = i;
        while (j > 0 && IsOpcodeChar(line[j - 1]) && !inHeader(j - 1)) j--;
        if (j == i) continue;                        // '=' with no identifier
        items.push_back({ j, false, j, i });
    }

    std::sort(items.begin(), items.end(),
              [](const Item& x, const Item& y) { return x.pos < y.pos; });

    for (size_t k = 0; k < items.size(); k++) {
        Token t;
        if (items[k].isHeader) {
            t.isHeader = true;
            t.name = line.substr(items[k].a + 1, items[k].b - items[k].a - 1);
        } else {
            t.name = line.substr(items[k].a, items[k].b - items[k].a);
            // An unquoted value runs to the start of the next item — the next
            // opcode name OR the next header — which is what lets a sample name
            // contain spaces ("sample=Bd Brush 1.wav lokey=36").
            const size_t vs  = items[k].b + 1;
            const size_t end = (k + 1 < items.size()) ? items[k + 1].pos
                                                      : line.size();
            t.value = (vs < end) ? line.substr(vs, end - vs) : std::string();
            while (!t.value.empty() && std::isspace((unsigned char)t.value.front()))
                t.value.erase(t.value.begin());
            while (!t.value.empty() && std::isspace((unsigned char)t.value.back()))
                t.value.pop_back();
        }
        for (char& c : t.name) c = (char)std::tolower((unsigned char)c);
        out->push_back(t);
    }
}

// ---------------------------------------------------------------- builder --

// Accumulates regions while walking the opcode stream, resolving each
// <region>'s effective opcodes from the enclosing <global>/<master>/<group>
// scopes and decoding each distinct sample file once.
class Builder {
public:
    Builder(const std::string& baseDir, LoadedInstrument* out)
        : fBaseDir(baseDir), fOut(out) {}

    void Header(const std::string& name) {
        Flush();                       // a new header closes any open <region>
        if (name == "region") {
            fInRegion = true;
            fRegionOps.clear();
        } else if (name == "group") {
            fGroupOps.clear();
        } else if (name == "master") {
            fMasterOps.clear();
            fGroupOps.clear();
        } else if (name == "global") {
            fGlobalOps.clear();
            fMasterOps.clear();
            fGroupOps.clear();
        }
        fScope = name;
    }

    void Opcode(const std::string& name, const std::string& value) {
        if (fInRegion)                 fRegionOps[name] = value;
        else if (fScope == "group")    fGroupOps[name]  = value;
        else if (fScope == "master")   fMasterOps[name] = value;
        else if (fScope == "global")   fGlobalOps[name] = value;
        else if (fScope == "control") {
            // <control> is file-level: default_path prefixes every later
            // sample=, and is itself relative to the SFZ's directory.
            if (name == "default_path") fDefaultPath = value;
        }
    }

    // Close the open <region>, if any.
    void Flush() {
        if (!fInRegion) return;
        fInRegion = false;

        OpcodeMap ops = fGlobalOps;
        for (const auto& kv : fMasterOps) ops[kv.first] = kv.second;
        for (const auto& kv : fGroupOps)  ops[kv.first] = kv.second;
        for (const auto& kv : fRegionOps) ops[kv.first] = kv.second;
        EmitRegion(ops);
    }

    int MissingSamples() const { return fMissing; }

private:
    // Look up an opcode, accepting a second spelling. SFZ v1 files in the wild
    // use both loop_mode/loopmode and off_by/offby.
    static const std::string* Find(const OpcodeMap& ops, const char* a,
                                   const char* b = nullptr) {
        auto it = ops.find(a);
        if (it != ops.end()) return &it->second;
        if (b) {
            it = ops.find(b);
            if (it != ops.end()) return &it->second;
        }
        return nullptr;
    }

    void EmitRegion(const OpcodeMap& ops) {
        const std::string* sample = Find(ops, "sample");
        if (!sample || sample->empty())
            return;                                  // nothing to play
        // "*sine" and friends: built-in generators, not files. Not supported.
        if ((*sample)[0] == '*')
            return;

        const int si = SampleIndex(*sample);
        if (si < 0) { fMissing++; return; }

        Region r;
        r.sampleIndex = si;

        if (const std::string* v = Find(ops, "key")) {
            const int k = ParseKey(*v, 60);
            r.loKey = r.hiKey = r.pitchKeycenter = k;
        }
        if (const std::string* v = Find(ops, "lokey")) r.loKey = ParseKey(*v, r.loKey);
        if (const std::string* v = Find(ops, "hikey")) r.hiKey = ParseKey(*v, r.hiKey);
        if (const std::string* v = Find(ops, "lovel")) r.loVel = (int)ParseInt(*v, r.loVel);
        if (const std::string* v = Find(ops, "hivel")) r.hiVel = (int)ParseInt(*v, r.hiVel);

        if (const std::string* v = Find(ops, "pitch_keycenter", "pitchkeycenter"))
            r.pitchKeycenter = ParseKey(*v, r.pitchKeycenter);
        if (const std::string* v = Find(ops, "pitch_keytrack", "pitchkeytrack"))
            r.pitchKeytrack = ParseFloat(*v, r.pitchKeytrack);

        // tune and transpose stack: transpose is semitones, tune is cents.
        float cents = 0.0f;
        if (const std::string* v = Find(ops, "tune", "pitch"))
            cents += ParseFloat(*v, 0.0f);
        if (const std::string* v = Find(ops, "transpose"))
            cents += 100.0f * ParseFloat(*v, 0.0f);
        r.tuneCents = cents;

        if (const std::string* v = Find(ops, "volume"))       r.volumeDb    = ParseFloat(*v, 0.0f);
        if (const std::string* v = Find(ops, "pan"))          r.pan         = ParseFloat(*v, 0.0f);
        if (const std::string* v = Find(ops, "amp_veltrack", "ampveltrack"))
            r.ampVeltrack = ParseFloat(*v, 100.0f);

        if (const std::string* v = Find(ops, "offset"))       r.offset      = ParseInt(*v, 0);
        if (const std::string* v = Find(ops, "end")) {
            const int64_t e = ParseInt(*v, -1);
            // end=-1 is SFZ's "disable this region", NOT "play to the end".
            if (e < 0) return;
            r.end = e;
        }
        if (const std::string* v = Find(ops, "delay"))        r.delaySec    = ParseFloat(*v, 0.0f);

        if (const std::string* v = Find(ops, "loop_mode", "loopmode")) {
            std::string m = *v;
            for (char& c : m) c = (char)std::tolower((unsigned char)c);
            if      (m == "one_shot")        r.loopMode = LoopMode::OneShot;
            else if (m == "loop_continuous") r.loopMode = LoopMode::LoopContinuous;
            else if (m == "loop_sustain")    r.loopMode = LoopMode::LoopSustain;
            else                             r.loopMode = LoopMode::NoLoop;
        }
        if (const std::string* v = Find(ops, "loop_start", "loopstart"))
            r.loopStart = ParseInt(*v, 0);
        if (const std::string* v = Find(ops, "loop_end", "loopend"))
            r.loopEnd = ParseInt(*v, -1);

        if (const std::string* v = Find(ops, "ampeg_delay"))   r.ampegDelay   = ParseFloat(*v, 0.0f);
        if (const std::string* v = Find(ops, "ampeg_attack"))  r.ampegAttack  = ParseFloat(*v, 0.0f);
        if (const std::string* v = Find(ops, "ampeg_hold"))    r.ampegHold    = ParseFloat(*v, 0.0f);
        if (const std::string* v = Find(ops, "ampeg_decay"))   r.ampegDecay   = ParseFloat(*v, 0.0f);
        if (const std::string* v = Find(ops, "ampeg_sustain")) r.ampegSustain = ParseFloat(*v, 100.0f);
        if (const std::string* v = Find(ops, "ampeg_release")) r.ampegRelease = ParseFloat(*v, 0.001f);

        // Round-robin. Without these a library that records several takes per
        // hit (lorand/hirand on a <group>) layers every take at once instead of
        // choosing one — several times too loud, and flammy.
        if (const std::string* v = Find(ops, "lorand")) r.loRand = ParseFloat(*v, 0.0f);
        if (const std::string* v = Find(ops, "hirand")) r.hiRand = ParseFloat(*v, 1.0f);
        if (const std::string* v = Find(ops, "seq_length", "seqlength"))
            r.seqLength = (int)ParseInt(*v, 1);
        if (const std::string* v = Find(ops, "seq_position", "seqposition"))
            r.seqPosition = (int)ParseInt(*v, 1);

        if (const std::string* v = Find(ops, "group"))          r.group = (int)ParseInt(*v, 0);
        if (const std::string* v = Find(ops, "off_by", "offby")) r.offBy = (int)ParseInt(*v, 0);

        fOut->regions.push_back(r);
    }

    // Decode a sample file once; later regions naming it reuse the index.
    //
    // Enforces the instrument memory budget: this loads every sample into RAM,
    // and large commercial libraries (Swirly Drums decodes to 2.3 GB) can
    // exceed what the machine has. Stopping with a clear message beats an OOM
    // abort — the regions already loaded still play.
    int SampleIndex(const std::string& rel) {
        std::string withDefault = rel;
        if (!fDefaultPath.empty()) {
            std::string dp = fDefaultPath;
            for (char& c : dp) if (c == '\\') c = '/';
            if (!dp.empty() && dp.back() != '/') dp += '/';
            withDefault = dp + rel;
        }
        const std::string full = ResolvePath(fBaseDir, withDefault);

        auto it = fSampleIds.find(full);
        if (it != fSampleIds.end()) return it->second;

        if (fOverBudget) return -1;

        // Size the sample BEFORE decoding it, so an oversized file is refused
        // without ever being read in — checking only afterwards let peak usage
        // reach the budget plus one whole sample.
        //
        // The two failure modes must stay distinct: not fitting the budget is a
        // PARTIAL load the user has to be told about, while a missing file is
        // just a dropped region. Asking LoadWavToMemory to enforce the cap
        // collapsed both into a plain false, which silently lost the warning.
        const int64_t need = WavDecodedBytes(full);
        if (need <= 0) {
            fSampleIds[full] = -1;      // missing/unreadable; don't retry
            return -1;
        }
        if (fBytes + need > kMaxInstrumentBytes) {
            fOverBudget = true;
            fSampleIds[full] = -1;
            return -1;
        }

        SampleData sd;
        if (!LoadWavToMemory(full, &sd, kMaxInstrumentBytes - fBytes)) {
            fSampleIds[full] = -1;
            return -1;
        }
        const int64_t bytes = (int64_t)sd.data.size() * (int64_t)sizeof(float);
        if (fBytes + bytes > kMaxInstrumentBytes) {
            fOverBudget = true;
            fSampleIds[full] = -1;
            return -1;
        }
        fBytes += bytes;
        const int idx = (int)fOut->samples.size();
        fOut->samples.push_back(std::move(sd));
        fSampleIds[full] = idx;
        return idx;
    }

public:
    bool OverBudget() const { return fOverBudget; }

private:

    std::string       fBaseDir;
    std::string       fDefaultPath;
    LoadedInstrument* fOut;
    OpcodeMap         fGlobalOps, fMasterOps, fGroupOps, fRegionOps;
    std::string       fScope;
    bool              fInRegion = false;
    int               fMissing  = 0;
    int64_t           fBytes      = 0;      // decoded so far, against the budget
    bool              fOverBudget = false;
    std::map<std::string, int> fSampleIds;
};

// Expand $variables defined by #define. SFZ substitution is plain textual
// replacement, longest name first so $kick10 isn't clobbered by $kick1.
//
// Bounded on purpose. A define whose VALUE names a define expanded later in the
// list gets substituted again on that later pass, so chaining defines with
// decreasing name lengths doubles the line per level — the classic "billion
// laughs". A 1 KB file measured at 20 s and climbing before this cap. Real
// libraries use defines for key numbers and sample folders, nowhere near it.
constexpr size_t kMaxExpandedLine = 64u * 1024;

void ExpandDefines(std::string* line,
                   const std::vector<std::pair<std::string, std::string>>& defs) {
    for (const auto& d : defs) {
        if (d.first.empty()) continue;
        size_t pos = 0;
        while ((pos = line->find(d.first, pos)) != std::string::npos) {
            if (line->size() - d.first.size() + d.second.size() > kMaxExpandedLine)
                return;                       // runaway expansion: stop here
            line->replace(pos, d.first.size(), d.second);
            pos += d.second.size();
        }
    }
}

// Walk one file's lines, following #include. `depth` bounds include recursion;
// a file that includes itself would otherwise spin forever.
void ParseStream(std::istream& in, const std::string& baseDir, Builder* b,
                 std::vector<std::pair<std::string, std::string>>* defs,
                 bool* inBlockComment, int depth, int* budget);

void ParseInclude(const std::string& rel, const std::string& baseDir, Builder* b,
                  std::vector<std::pair<std::string, std::string>>* defs,
                  int depth, int* budget) {
    // Depth alone does NOT bound the work: a file including itself 12 times is
    // parsed 12^9 times at depth 8, which never finishes. Bound the TOTAL
    // number of files opened as well. Real libraries (Swirly Drums is the
    // heaviest here) use a few hundred includes across a keymap tree.
    if (depth > 8 || !budget || *budget <= 0) return;
    (*budget)--;
    std::string p = rel;
    // #include "file.sfz" — strip the quotes.
    while (!p.empty() && (p.front() == '"' || p.front() == ' ')) p.erase(p.begin());
    while (!p.empty() && (p.back() == '"' || p.back() == ' ')) p.pop_back();
    const std::string full = ResolvePath(baseDir, p);
    std::ifstream f(full);
    if (!f) return;
    bool inBlock = false;
    ParseStream(f, DirOf(full), b, defs, &inBlock, depth + 1, budget);
}

void ParseStream(std::istream& in, const std::string& baseDir, Builder* b,
                 std::vector<std::pair<std::string, std::string>>* defs,
                 bool* inBlockComment, int depth, int* budget) {
    std::string raw;
    while (std::getline(in, raw)) {
        if (!raw.empty() && raw.back() == '\r') raw.pop_back();   // CRLF files
        std::string line = StripComments(raw, inBlockComment);

        // Directives own their whole line.
        size_t ns = line.find_first_not_of(" \t");
        if (ns != std::string::npos && line[ns] == '#') {
            std::istringstream ds(line.substr(ns + 1));
            std::string what;
            ds >> what;
            if (what == "define") {
                std::string name, value;
                ds >> name >> value;
                if (!name.empty() && name[0] == '$' && defs->size() < 4096) {
                    defs->push_back({ name, value });
                    // Longest first, so $kick10 wins over $kick1.
                    std::sort(defs->begin(), defs->end(),
                              [](const std::pair<std::string, std::string>& a,
                                 const std::pair<std::string, std::string>& c) {
                                  return a.first.size() > c.first.size();
                              });
                }
            } else if (what == "include") {
                std::string rest;
                std::getline(ds, rest);
                ExpandDefines(&rest, *defs);
                ParseInclude(rest, baseDir, b, defs, depth, budget);
            }
            continue;
        }

        if (!defs->empty()) ExpandDefines(&line, *defs);

        std::vector<Token> toks;
        LexLine(line, &toks);
        for (const Token& t : toks) {
            if (t.isHeader) b->Header(t.name);
            else            b->Opcode(t.name, t.value);
        }
    }
}

} // namespace

bool ParseSfzText(const std::string& text, const std::string& baseDir,
                  LoadedInstrument* out, std::string* error,
                  std::string* warning) {
    if (!out) return false;
    std::istringstream in(text);
    Builder b(baseDir, out);
    std::vector<std::pair<std::string, std::string>> defs;
    bool inBlock = false;
    int  budget  = 4096;          // total #include files, see ParseInclude
    ParseStream(in, baseDir, &b, &defs, &inBlock, 0, &budget);
    b.Flush();
    FinalizeInstrument(out);

    if (b.OverBudget()) {
        // PARTIAL success: what loaded is playable, but the kit is incomplete.
        // This belongs in `warning`, not `error` — the caller still gets the
        // instrument, and must be able to tell the two apart.
        if (warning) {
            char msg[192];
            std::snprintf(msg, sizeof msg,
                          "instrument exceeds the %lld MB sample budget; loaded "
                          "%zu of its samples",
                          (long long)(kMaxInstrumentBytes / (1024 * 1024)),
                          out->samples.size());
            *warning = msg;
        }
    } else if (out->regions.empty() && b.MissingSamples() > 0 && error) {
        char msg[128];
        std::snprintf(msg, sizeof msg,
                      "%d sample file(s) referenced by the SFZ are missing",
                      b.MissingSamples());
        *error = msg;
    }
    return true;
}

bool LoadSfz(const std::string& path, LoadedInstrument* out, std::string* error,
             std::string* warning) {
    if (!out) return false;
    std::ifstream f(path);
    if (!f) {
        if (error) *error = "cannot open '" + path + "'";
        return false;
    }
    std::stringstream ss;
    ss << f.rdbuf();

    const size_t slash = path.find_last_of("/\\");
    out->name = (slash == std::string::npos) ? path : path.substr(slash + 1);

    return ParseSfzText(ss.str(), DirOf(path), out, error, warning);
}

} // namespace daw
