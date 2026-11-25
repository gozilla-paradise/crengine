/** \file thaiwordbreak.cpp
    \brief Dictionary based line breaking opportunities for Thai text

    See thaiwordbreak.h for the purpose and the guarantees.

    How it works, for each run of Thai chars:

    1. The run is normalized (some common decomposed spellings of SARA AM
       are recomposed) and each char is stored as a single byte (its
       offset in the Thai Unicode block, 0x01..0x4E).

    2. Cluster boundaries: words can only start and end on them, so
       there is never a break before a dependent vowel, tone mark or
       other combining mark, or after a leading vowel. A word can end
       before MAI YAMOK or PAIYANNOI, but we never break before them.

    3. A graph is built over cluster boundaries: an edge for each
       dictionary word found at a position (known), and an edge to the
       next cluster boundary (unknown, when no word explains this text).
       The best paths minimize first the number of unknown chars, then
       the number of words (so, longer words are preferred, which is the
       "maximal matching" idea).

    4. Instead of picking one best path (which would be arbitrary when
       the text is ambiguous: ตากลม = ตา|กลม or ตาก|ลม), we compute
       forward and backward best costs, and keep as candidate breaks only
       the boundaries that all best paths go through (no best edge jumps
       over them). And we only allow a break when the words on both sides
       of it are known, and are not next to some unknown text (as text
       not in the dictionary is often a name or a foreign word, that the
       dictionary would otherwise cut into small unrelated words).

    The dictionary is a sorted array of byte strings, all packed in a
    single blob. Finding all the words starting at some position is done
    by narrowing the range of words sharing the prefix, char by char.

    CoolReader Engine / KOReader
    This source code is distributed under the terms of
    GNU General Public License
    See LICENSE file for details
*/

#include "../include/thaiwordbreak.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include <algorithm>

namespace ThaiWordBreak {

namespace {

// Thai chars are stored as their offset in the Thai Unicode block
// (0x01..0x4E for the chars we handle), and 0 as a word terminator.
#define TWB_NB_BYTES 0x50

inline unsigned char toByte(uint32_t c) {
    return (unsigned char)(c - 0x0E00);
}

// Chars that never start a word, and before which we never break:
// dependent vowels, tone marks and other combining marks, SARA A, SARA AA,
// SARA AM, LAKKHANGYAO.
inline bool isFollowingByte(unsigned char b) {
    return (b >= 0x30 && b <= 0x3A) || b == 0x45 || (b >= 0x47 && b <= 0x4E);
}

// MAI YAMOK (repetition) and PAIYANNOI (abbreviation): they follow a
// word, which can end before them, but we never break before them.
inline bool isWordMarkByte(unsigned char b) {
    return b == 0x46 || b == 0x2F;
}

// Leading vowels (SARA E, SARA AE, SARA O, SARA AI MAIMUAN, SARA AI MAIMALAI),
// after which we never break.
inline bool isLeadingByte(unsigned char b) {
    return b >= 0x40 && b <= 0x44;
}

inline bool isToneMark(uint32_t c) {
    return c >= 0x0E48 && c <= 0x0E4B;
}

// Normalize Thai text in[0..n) (all Thai chars) into bytes, recomposing
// SARA AM when typed as NIKHAHIT + SARA AA (possibly with a tone mark
// in between or before). map[k] is the index in 'in' of the first char
// that gave out[k], and map[out_len] = n.
// Returns the normalized length.
template <typename CharT>
int normalize(const CharT * in, int n, unsigned char * out, int * map) {
    int k = 0;
    int i = 0;
    while ( i < n ) {
        uint32_t c = (uint32_t)in[i];
        if ( c == 0x0E4D ) {
            if ( i+1 < n && (uint32_t)in[i+1] == 0x0E32 ) { // NIKHAHIT + SARA AA
                map[k] = i;
                out[k++] = 0x33; // SARA AM
                i += 2;
                continue;
            }
            if ( i+2 < n && isToneMark((uint32_t)in[i+1]) && (uint32_t)in[i+2] == 0x0E32 ) {
                // NIKHAHIT + tone mark + SARA AA => tone mark + SARA AM
                map[k] = i;
                out[k++] = toByte((uint32_t)in[i+1]);
                map[k] = i;
                out[k++] = 0x33;
                i += 3;
                continue;
            }
        }
        map[k] = i;
        out[k++] = toByte(c);
        i++;
    }
    map[k] = n;
    return k;
}

class Dict {
    std::vector<unsigned char> _blob; // all words, each terminated by a 0
    std::vector<uint32_t> _offs;      // offset in _blob of each word, sorted
    std::vector<unsigned char> _weak; // per word: 1 if weak (empty if none)
    uint32_t _first_lo[TWB_NB_BYTES]; // range of words starting with
    uint32_t _first_hi[TWB_NB_BYTES]; // each first byte
public:
    Dict() { clear(); }

    void clear() {
        std::vector<unsigned char>().swap(_blob);
        std::vector<uint32_t>().swap(_offs);
        std::vector<unsigned char>().swap(_weak);
        memset(_first_lo, 0, sizeof(_first_lo));
        memset(_first_hi, 0, sizeof(_first_hi));
    }

    size_t size() const { return _offs.size(); }

    // Build from a list of byte encoded words (which will be sorted in place)
    void build(std::vector<std::string> & words) {
        clear();
        std::sort(words.begin(), words.end());
        words.erase(std::unique(words.begin(), words.end()), words.end());
        size_t total = 0;
        for ( size_t i=0; i<words.size(); i++ )
            total += words[i].size() + 1;
        _blob.reserve(total);
        _offs.reserve(words.size());
        for ( size_t i=0; i<words.size(); i++ ) {
            unsigned char b = (unsigned char)words[i][0];
            uint32_t idx = (uint32_t)_offs.size();
            if ( _first_lo[b] == _first_hi[b] )
                _first_lo[b] = idx;
            _first_hi[b] = idx + 1;
            _offs.push_back((uint32_t)_blob.size());
            _blob.insert(_blob.end(), words[i].begin(), words[i].end());
            _blob.push_back(0);
        }
    }

    // Flag as weak the words (byte encoded) of the provided list that are
    // in this dict. Returns the number of words flagged.
    int markWeak(const std::vector<std::string> & words) {
        _weak.assign(_offs.size(), 0);
        int nb = 0;
        const unsigned char * blob = _blob.data();
        for ( size_t i=0; i<words.size(); i++ ) {
            const char * w = words[i].c_str();
            // binary search for the exact word
            uint32_t a = 0, z = (uint32_t)_offs.size();
            while ( a < z ) {
                uint32_t m = a + (z-a)/2;
                if ( strcmp((const char *)blob + _offs[m], w) < 0 ) a = m+1; else z = m;
            }
            if ( a < _offs.size() && strcmp((const char *)blob + _offs[a], w) == 0 && !_weak[a] ) {
                _weak[a] = 1;
                nb++;
            }
        }
        return nb;
    }

    // Find all the words that are a prefix of s[0..n), and call f(len, weak)
    // for each of them, by increasing length.
    template <typename F>
    void forEachPrefix(const unsigned char * s, int n, F f) const {
        if ( n <= 0 || _offs.empty() )
            return;
        unsigned char b = s[0];
        if ( b >= TWB_NB_BYTES )
            return;
        uint32_t lo = _first_lo[b];
        uint32_t hi = _first_hi[b];
        if ( lo >= hi )
            return;
        const unsigned char * blob = _blob.data();
        const uint32_t * offs = _offs.data();
        const bool has_weak = !_weak.empty();
        // All words in [lo, hi) share the prefix s[0..d). Being sorted, the
        // byte at index d is non-decreasing over that range (and is 0 for
        // the one word, if any, that is exactly that prefix, which is then
        // the first one in that range).
        if ( blob[offs[lo]+1] == 0 )
            f(1, has_weak && _weak[lo]);
        for ( int d=1; d<n; d++ ) {
            unsigned char c = s[d];
            uint32_t a = lo, z = hi;
            while ( a < z ) { // lower bound
                uint32_t m = a + (z-a)/2;
                if ( blob[offs[m]+d] < c ) a = m+1; else z = m;
            }
            lo = a;
            z = hi;
            while ( a < z ) { // upper bound
                uint32_t m = a + (z-a)/2;
                if ( blob[offs[m]+d] <= c ) a = m+1; else z = m;
            }
            hi = a;
            if ( lo >= hi )
                return;
            if ( blob[offs[lo]+d+1] == 0 )
                f(d+1, has_weak && _weak[lo]);
        }
    }
};

// Decode one UTF-8 sequence at s[0..n), store code point in *cp,
// return its length, or 0 if invalid.
int decodeUtf8(const unsigned char * s, size_t n, uint32_t * cp) {
    if ( n == 0 )
        return 0;
    unsigned char c = s[0];
    if ( c < 0x80 ) { *cp = c; return 1; }
    int len;
    uint32_t v;
    if ( (c & 0xE0) == 0xC0 )      { len = 2; v = c & 0x1F; }
    else if ( (c & 0xF0) == 0xE0 ) { len = 3; v = c & 0x0F; }
    else if ( (c & 0xF8) == 0xF0 ) { len = 4; v = c & 0x07; }
    else return 0;
    if ( (size_t)len > n )
        return 0;
    for ( int i=1; i<len; i++ ) {
        if ( (s[i] & 0xC0) != 0x80 )
            return 0;
        v = (v << 6) | (s[i] & 0x3F);
    }
    *cp = v;
    return len;
}

// Parse a word list (UTF-8, one word per line, '#' starts a comment line).
// Lines that are not made only of Thai chars (after trimming spaces)
// are skipped, as they could never match inside a run of Thai chars.
// Returns the number of words added to 'words'.
int parseWordList(const char * buf, size_t size, std::vector<std::string> & words, uint32_t * hash) {
    const unsigned char * p = (const unsigned char *)buf;
    const unsigned char * end = p + size;
    if ( size >= 3 && p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF )
        p += 3; // skip UTF-8 BOM
    std::vector<uint32_t> cps;
    std::vector<unsigned char> bytes;
    std::vector<int> map;
    int nb_added = 0;
    while ( p < end ) {
        const unsigned char * eol = p;
        while ( eol < end && *eol != '\n' && *eol != '\r' )
            eol++;
        // Trim spaces and tabs
        const unsigned char * s = p;
        const unsigned char * e = eol;
        while ( s < e && (*s == ' ' || *s == '\t') ) s++;
        while ( e > s && (e[-1] == ' ' || e[-1] == '\t') ) e--;
        p = eol;
        while ( p < end && (*p == '\n' || *p == '\r') )
            p++;
        if ( s == e || *s == '#' )
            continue;
        cps.clear();
        bool ok = true;
        while ( s < e ) {
            uint32_t cp;
            int len = decodeUtf8(s, e - s, &cp);
            if ( len == 0 || !isThaiChar(cp) ) {
                ok = false;
                break;
            }
            cps.push_back(cp);
            s += len;
        }
        if ( !ok || cps.empty() )
            continue;
        bytes.resize(cps.size());
        map.resize(cps.size() + 1);
        int n = normalize(cps.data(), (int)cps.size(), bytes.data(), map.data());
        words.push_back(std::string((const char *)bytes.data(), n));
        if ( hash ) {
            for ( int i=0; i<n; i++ )
                *hash = *hash * 31 + bytes[i];
            *hash = *hash * 31 + 0xFF;
        }
        nb_added++;
    }
    return nb_added;
}

bool readFile(const char * path, std::vector<char> & buf) {
    FILE * f = fopen(path, "rb");
    if ( !f )
        return false;
    buf.clear();
    char chunk[65536];
    size_t n;
    while ( (n = fread(chunk, 1, sizeof(chunk), f)) > 0 )
        buf.insert(buf.end(), chunk, chunk + n);
    bool ok = !ferror(f);
    fclose(f);
    return ok;
}

// Module state (not thread safe: crengine text layout is single threaded)
std::string _main_path;
std::string _weak_path;
bool _main_loaded = false;
Dict _main_dict;
Dict _user_dict;
uint32_t _user_hash = 0;

void loadMainDict() {
    _main_loaded = true;
    std::vector<char> buf;
    if ( !readFile(_main_path.c_str(), buf) ) {
        printf("CRE WARNING: ThaiWordBreak cannot read word list: %s\n", _main_path.c_str());
        return;
    }
    std::vector<std::string> words;
    parseWordList(buf.data(), buf.size(), words, NULL);
    _main_dict.build(words);
    if ( !_weak_path.empty() ) {
        if ( readFile(_weak_path.c_str(), buf) ) {
            words.clear();
            parseWordList(buf.data(), buf.size(), words, NULL);
            _main_dict.markWeak(words);
        }
        else {
            printf("CRE WARNING: ThaiWordBreak cannot read weak word list: %s\n", _weak_path.c_str());
        }
    }
}

// Reused buffers
std::vector<unsigned char> _s;   // normalized run
std::vector<int> _map;           // normalized index => run index
std::vector<unsigned char> _bnd; // cluster boundary flags
struct Edge {
    int from;
    int to;
    bool known;
    bool weak; // a known but weak word: no break on either side of it
};
std::vector<Edge> _edges;
std::vector<int> _stamp;         // to dedup matches from both dicts
std::vector<int64_t> _fwd;       // best cost from 0 to node
std::vector<int64_t> _bwd;       // best cost from node to end
std::vector<int> _cross;         // difference array: best edges strictly crossing node
std::vector<unsigned char> _in_unk;   // some best edge ending at node is unknown
std::vector<unsigned char> _out_unk;  // some best edge starting at node is unknown
std::vector<unsigned char> _left_bad; // some best edge ending at node starts at a node with _in_unk
std::vector<unsigned char> _right_bad;// some best edge starting at node ends at a node with _out_unk
std::vector<unsigned char> _weak_adj; // some best edge ending or starting at node is a weak word

// Costs are (unknown chars, words), compared lexicographically:
// packed into an int64 so they can be added and compared directly.
const int64_t COST_INF = (int64_t)1 << 60;
inline int64_t edgeCost(const Edge & e) {
    return e.known ? 1 : ((int64_t)(e.to - e.from) << 32);
}

// Segment the normalized run s[0..n), and push in 'breaks' the
// normalized indexes k (1 <= k < n) where a break is allowed
// between s[k-1] and s[k].
void segmentRun(const unsigned char * s, int n, std::vector<int> & breaks) {
    if ( n < 2 )
        return;
    _bnd.assign(n+1, 1);
    for ( int k=1; k<n; k++ ) {
        if ( isFollowingByte(s[k]) || isLeadingByte(s[k-1]) )
            _bnd[k] = 0;
    }
    // Build edges, grouped by increasing 'from'
    _edges.clear();
    _stamp.assign(n+1, -1);
    for ( int i=0; i<n; i++ ) {
        if ( !_bnd[i] )
            continue;
        auto addKnown = [&](int len, bool weak) {
            int j = i + len;
            if ( _bnd[j] && _stamp[j] != i ) {
                _stamp[j] = i;
                _edges.push_back({i, j, true, weak});
            }
        };
        // User words first: if also in the main dict, they are never weak
        _user_dict.forEachPrefix(s+i, n-i, addKnown);
        _main_dict.forEachPrefix(s+i, n-i, addKnown);
        if ( isWordMarkByte(s[i]) && _bnd[i+1] && _stamp[i+1] != i ) {
            // MAI YAMOK and PAIYANNOI are known words on their own
            _stamp[i+1] = i;
            _edges.push_back({i, i+1, true, false});
        }
        // Unknown text up to the next cluster boundary
        int j = i + 1;
        while ( !_bnd[j] )
            j++;
        _edges.push_back({i, j, false, false});
    }
    int nb_edges = (int)_edges.size();
    // Forward best costs (edges are sorted by 'from', and from < to)
    _fwd.assign(n+1, COST_INF);
    _fwd[0] = 0;
    for ( int e=0; e<nb_edges; e++ ) {
        const Edge & ed = _edges[e];
        if ( _fwd[ed.from] >= COST_INF )
            continue;
        int64_t c = _fwd[ed.from] + edgeCost(ed);
        if ( c < _fwd[ed.to] )
            _fwd[ed.to] = c;
    }
    // Backward best costs
    _bwd.assign(n+1, COST_INF);
    _bwd[n] = 0;
    for ( int e=nb_edges-1; e>=0; e-- ) {
        const Edge & ed = _edges[e];
        if ( _bwd[ed.to] >= COST_INF )
            continue;
        int64_t c = _bwd[ed.to] + edgeCost(ed);
        if ( c < _bwd[ed.from] )
            _bwd[ed.from] = c;
    }
    const int64_t best = _fwd[n];
    // Look at best edges (those on some best path)
    _cross.assign(n+2, 0);
    _in_unk.assign(n+1, 0);
    _out_unk.assign(n+1, 0);
    for ( int e=0; e<nb_edges; e++ ) {
        const Edge & ed = _edges[e];
        if ( _fwd[ed.from] >= COST_INF || _bwd[ed.to] >= COST_INF )
            continue;
        if ( _fwd[ed.from] + edgeCost(ed) + _bwd[ed.to] != best )
            continue;
        if ( ed.to - ed.from > 1 ) { // nodes from+1 .. to-1 are jumped over
            _cross[ed.from+1]++;
            _cross[ed.to]--;
        }
        if ( !ed.known ) {
            _out_unk[ed.from] = 1;
            _in_unk[ed.to] = 1;
        }
    }
    _left_bad.assign(n+1, 0);
    _right_bad.assign(n+1, 0);
    _weak_adj.assign(n+1, 0);
    for ( int e=0; e<nb_edges; e++ ) {
        const Edge & ed = _edges[e];
        if ( _fwd[ed.from] >= COST_INF || _bwd[ed.to] >= COST_INF )
            continue;
        if ( _fwd[ed.from] + edgeCost(ed) + _bwd[ed.to] != best )
            continue;
        if ( ed.weak ) {
            _weak_adj[ed.from] = 1;
            _weak_adj[ed.to] = 1;
        }
        if ( _in_unk[ed.from] )
            _left_bad[ed.to] = 1;
        if ( _out_unk[ed.to] )
            _right_bad[ed.from] = 1;
    }
    int crossing = 0;
    for ( int k=1; k<n; k++ ) {
        crossing += _cross[k];
        if ( !_bnd[k] || isWordMarkByte(s[k]) )
            continue;
        if ( crossing > 0 ) // some best path does not go through k
            continue;
        if ( _fwd[k] + _bwd[k] != best ) // (should not happen if not crossed)
            continue;
        if ( _in_unk[k] || _out_unk[k] ) // unknown text on one side
            continue;
        if ( _left_bad[k] || _right_bad[k] ) // known word next to unknown text
            continue;
        if ( _weak_adj[k] ) // weak word on one side
            continue;
        breaks.push_back(k);
    }
}

} // anonymous namespace

void setMainDictPath(const char * path, const char * weak_path) {
    std::string p = path ? path : "";
    std::string wp = weak_path ? weak_path : "";
    if ( p == _main_path && wp == _weak_path )
        return;
    _main_path = p;
    _weak_path = wp;
    _main_dict.clear();
    _main_loaded = false;
}

int setUserDictFromBuffer(const char * buf, size_t size) {
    std::vector<std::string> words;
    uint32_t hash = 0;
    if ( buf && size > 0 )
        parseWordList(buf, size, words, &hash);
    _user_dict.build(words);
    _user_hash = _user_dict.size() > 0 ? hash : 0;
    return (int)_user_dict.size();
}

int setUserDict(const char * path) {
    if ( !path || !path[0] ) {
        _user_dict.clear();
        _user_hash = 0;
        return 0;
    }
    std::vector<char> buf;
    if ( !readFile(path, buf) ) {
        _user_dict.clear();
        _user_hash = 0;
        return -1;
    }
    return setUserDictFromBuffer(buf.data(), buf.size());
}

uint32_t getHash() {
    if ( _main_path.empty() )
        return 0;
    uint32_t hash = 0x7A1;
    for ( size_t i=0; i<_main_path.size(); i++ )
        hash = hash * 31 + (unsigned char)_main_path[i];
    for ( size_t i=0; i<_weak_path.size(); i++ )
        hash = hash * 31 + (unsigned char)_weak_path[i];
    hash = hash * 31 + _user_hash;
    return hash;
}

bool isEnabled() {
    return !_main_path.empty();
}

bool hasThai(const char32_t * text, int len) {
    for ( int i=0; i<len; i++ ) {
        if ( isThaiChar((uint32_t)text[i]) )
            return true;
    }
    return false;
}

int findBreaks(const char32_t * text, int len, break_cb_t callback, void * ctx) {
    if ( _main_path.empty() || !text || len < 2 )
        return 0;
    int nb_breaks = 0;
    std::vector<int> breaks;
    int i = 0;
    while ( i < len ) {
        if ( !isThaiChar((uint32_t)text[i]) ) {
            i++;
            continue;
        }
        int start = i;
        while ( i < len && isThaiChar((uint32_t)text[i]) )
            i++;
        int run_len = i - start;
        if ( run_len < 2 )
            continue;
        if ( !_main_loaded )
            loadMainDict();
        if ( _main_dict.size() == 0 && _user_dict.size() == 0 )
            return nb_breaks;
        _s.resize(run_len);
        _map.resize(run_len + 1);
        int n = normalize(text + start, run_len, _s.data(), _map.data());
        breaks.clear();
        segmentRun(_s.data(), n, breaks);
        for ( size_t b=0; b<breaks.size(); b++ ) {
            // Break between normalized s[k-1] and s[k], so after the
            // original char just before the one that gave s[k].
            int pos = start + _map[breaks[b]] - 1;
            callback(pos, ctx);
            nb_breaks++;
        }
    }
    return nb_breaks;
}

void release() {
    // The user dict is kept, as it is part of the configuration (and of getHash())
    _main_dict.clear();
    _main_loaded = false;
    std::vector<unsigned char>().swap(_s);
    std::vector<int>().swap(_map);
    std::vector<unsigned char>().swap(_bnd);
    std::vector<Edge>().swap(_edges);
    std::vector<int>().swap(_stamp);
    std::vector<int64_t>().swap(_fwd);
    std::vector<int64_t>().swap(_bwd);
    std::vector<int>().swap(_cross);
    std::vector<unsigned char>().swap(_in_unk);
    std::vector<unsigned char>().swap(_out_unk);
    std::vector<unsigned char>().swap(_left_bad);
    std::vector<unsigned char>().swap(_right_bad);
    std::vector<unsigned char>().swap(_weak_adj);
}

}
