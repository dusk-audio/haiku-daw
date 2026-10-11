// Base64 — the transport encoding for opaque byte/string blobs in the project
// file.
//
// Why it exists: a plugin's saved state (EffectDesc.state) is a multi-line TTL
// document with quotes, braces and newlines in it, and ProjectIO is one record
// per line with a backslash-escaping scheme that was never meant to carry
// kilobytes of nested text. Base64 keeps each state on exactly one line, in one
// whitespace-free token, so the line parser needs no new quoting rules and the
// `fxstate` line can carry any blob a plugin produces.
//
// Kit-free (STL only) and header-only, so the codec is host-tested on its own —
// it is the one piece of this feature with no lilv in it and no excuse for a
// Haiku-only test.
#pragma once

#include <cstddef>
#include <string>

namespace daw {

namespace base64_detail {
inline const char* Alphabet() {
    return "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
}
// -1 = not a base64 character.
inline int Value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}
} // namespace base64_detail

// Standard base64 with '=' padding, no line breaks. Empty in -> empty out.
inline std::string Base64Encode(const std::string& in) {
    const char* A = base64_detail::Alphabet();
    std::string out;
    out.reserve(((in.size() + 2) / 3) * 4);
    size_t i = 0;
    for (; i + 3 <= in.size(); i += 3) {
        const unsigned v = ((unsigned char)in[i] << 16)
                         | ((unsigned char)in[i + 1] << 8)
                         |  (unsigned char)in[i + 2];
        out += A[(v >> 18) & 63];
        out += A[(v >> 12) & 63];
        out += A[(v >> 6) & 63];
        out += A[v & 63];
    }
    const size_t rem = in.size() - i;
    if (rem == 1) {
        const unsigned v = (unsigned char)in[i] << 16;
        out += A[(v >> 18) & 63];
        out += A[(v >> 12) & 63];
        out += "==";
    } else if (rem == 2) {
        const unsigned v = ((unsigned char)in[i] << 16)
                         | ((unsigned char)in[i + 1] << 8);
        out += A[(v >> 18) & 63];
        out += A[(v >> 12) & 63];
        out += A[(v >> 6) & 63];
        out += '=';
    }
    return out;
}

// Strict decode: a non-alphabet character, a bad length, or padding in the wrong
// place fails (returns false and leaves `out` untouched). Strict on purpose —
// this decodes a file, and a half-decoded blob would be handed to a plugin as if
// it were its state. Callers treat a failure as "no state on this line", the
// same way ProjectIO treats an `fxin` naming a slot that does not exist.
inline bool Base64Decode(const std::string& in, std::string* out) {
    if (!out) return false;
    if (in.empty()) { out->clear(); return true; }
    if (in.size() % 4 != 0) return false;

    std::string decoded;
    decoded.reserve((in.size() / 4) * 3);
    for (size_t i = 0; i < in.size(); i += 4) {
        const bool last = (i + 4 == in.size());
        int v[4];
        int pad = 0;
        for (int k = 0; k < 4; k++) {
            const char c = in[i + (size_t)k];
            if (c == '=') {
                // Padding is legal only in the final group's last one or two
                // chars, and only where nothing follows it.
                if (!last || k < 2) return false;
                v[k] = 0;
                pad++;
            } else {
                if (pad > 0) return false;   // a character after '='
                v[k] = base64_detail::Value(c);
                if (v[k] < 0) return false;
            }
        }
        const unsigned n = ((unsigned)v[0] << 18) | ((unsigned)v[1] << 12)
                         | ((unsigned)v[2] << 6)  |  (unsigned)v[3];
        decoded += (char)((n >> 16) & 0xff);
        if (pad < 2) decoded += (char)((n >> 8) & 0xff);
        if (pad < 1) decoded += (char)(n & 0xff);
    }
    *out = std::move(decoded);
    return true;
}

} // namespace daw
