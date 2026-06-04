// er_version_check.h — pre-release-aware semver range check for the `versions` contract (spec-3)
//
// The client receives a `versions` range over the AP protocol and must accept/reject its own
// contract version against it. Implemented explicitly to node-semver semantics rather than trusting
// a C++ lib, because the load-bearing rule below isn't replicated by all of them. Acceptance vectors
// live in tests.cpp (verified against node-semver v22).
//
// Lockstep phase emits ">=0.1.0-beta.1 <0.1.0-beta.2"; graduates to ">=0.1.0 <0.2.0" at freeze.
// The client's OWN contract version stays a pre-release (0.1.0-beta.1) during lockstep and flips to
// a release (0.1.0) at freeze, in step with the apworld range and the randomizer's bake constant.
//
// THE RULE (node-semver, includePrerelease = false): a version carrying a pre-release tag satisfies
// a range only if some comparator in the set shares its exact [major,minor,patch] AND also carries a
// pre-release. This is why a naive ">=0.1.0" rejects "0.1.0-beta.1", and why includePrerelease must
// stay OFF (it would let a future-breaking "0.2.0-beta.1" leak through ">=0.1.0 <0.2.0").
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <stdexcept>
#include <cctype>

namespace er_ap {

struct SemVer {
    int major = 0, minor = 0, patch = 0;
    std::vector<std::string> prerelease;       // dot-separated ids; empty => release
    bool hasPre() const { return !prerelease.empty(); }
    bool sameCore(const SemVer& o) const {
        return major == o.major && minor == o.minor && patch == o.patch;
    }
};

inline std::vector<std::string> splitDots(const std::string& s) {
    std::vector<std::string> out; std::string cur;
    for (char c : s) { if (c == '.') { out.push_back(cur); cur.clear(); } else cur.push_back(c); }
    out.push_back(cur);
    return out;
}

inline bool isNumericId(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s) if (!std::isdigit(static_cast<unsigned char>(c))) return false;
    return true;
}

inline SemVer parseSemVer(const std::string& in) {
    std::string s = in;
    auto plus = s.find('+');                    // strip build metadata
    if (plus != std::string::npos) s = s.substr(0, plus);
    SemVer v;
    auto dash = s.find('-');
    std::string core = (dash == std::string::npos) ? s : s.substr(0, dash);
    if (dash != std::string::npos) v.prerelease = splitDots(s.substr(dash + 1));
    auto parts = splitDots(core);
    if (parts.size() != 3) throw std::invalid_argument("bad semver core: " + in);
    v.major = std::stoi(parts[0]); v.minor = std::stoi(parts[1]); v.patch = std::stoi(parts[2]);
    return v;
}

// pre-release precedence: -1 / 0 / 1
inline int comparePre(const std::vector<std::string>& a, const std::vector<std::string>& b) {
    if (a.empty() && b.empty()) return 0;
    if (a.empty()) return 1;                    // release > pre-release
    if (b.empty()) return -1;
    size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; ++i) {
        const std::string& x = a[i]; const std::string& y = b[i];
        bool xn = isNumericId(x), yn = isNumericId(y);
        if (xn && yn) {
            long xv = std::stol(x), yv = std::stol(y);
            if (xv != yv) return xv < yv ? -1 : 1;
        } else if (xn != yn) {
            return xn ? -1 : 1;                  // numeric ids rank below alphanumeric
        } else if (x != y) {
            return x < y ? -1 : 1;
        }
    }
    if (a.size() != b.size()) return a.size() < b.size() ? -1 : 1;  // fewer fields ranks lower
    return 0;
}

inline int compareSemVer(const SemVer& a, const SemVer& b) {
    if (a.major != b.major) return a.major < b.major ? -1 : 1;
    if (a.minor != b.minor) return a.minor < b.minor ? -1 : 1;
    if (a.patch != b.patch) return a.patch < b.patch ? -1 : 1;
    return comparePre(a.prerelease, b.prerelease);
}

struct Comparator { std::string op; SemVer ver; };

// whitespace-separated comparators (single AND set; no '||'), each "<op><version>",
// op in { >=, <=, >, <, = }; a bare version means "=".
inline std::vector<Comparator> parseRange(const std::string& range) {
    std::vector<Comparator> out; std::string tok;
    auto flush = [&]() {
        if (tok.empty()) return;
        std::string op = "=", v = tok;
        if (tok.size() >= 2 && (tok.compare(0, 2, ">=") == 0 || tok.compare(0, 2, "<=") == 0)) {
            op = tok.substr(0, 2); v = tok.substr(2);
        } else if (tok[0] == '>' || tok[0] == '<') {
            op = std::string(1, tok[0]); v = tok.substr(1);
        } else if (tok[0] == '=') {
            op = "="; v = tok.substr(1);
        }
        out.push_back({op, parseSemVer(v)});
        tok.clear();
    };
    for (char c : range) { if (std::isspace(static_cast<unsigned char>(c))) flush(); else tok.push_back(c); }
    flush();
    return out;
}

inline bool applyOp(int cmp, const std::string& op) {
    if (op == ">=") return cmp >= 0;
    if (op == ">")  return cmp > 0;
    if (op == "<=") return cmp <= 0;
    if (op == "<")  return cmp < 0;
    return cmp == 0;  // "="
}

// node-semver satisfies, includePrerelease = false.
inline bool versionSatisfies(const std::string& versionStr, const std::string& rangeStr) {
    SemVer v = parseSemVer(versionStr);
    std::vector<Comparator> comps = parseRange(rangeStr);
    for (const auto& c : comps)
        if (!applyOp(compareSemVer(v, c.ver), c.op)) return false;
    if (v.hasPre()) {                            // the pre-release-in-range gate
        bool allowed = false;
        for (const auto& c : comps)
            if (c.ver.hasPre() && c.ver.sameCore(v)) { allowed = true; break; }
        if (!allowed) return false;
    }
    return true;
}

} // namespace er_ap
