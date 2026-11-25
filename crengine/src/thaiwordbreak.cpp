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

    The dictionary is a trie, stored in flat arrays (the children of a
    node are contiguous, so looking for the next char reads a few
    consecutive bytes). Finding all the words starting at some position
    is a single walk down the trie, char by char.

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
    // A trie: node 0 is the root, and the children of a node are the
    // _nchild[k] nodes (consecutive ids) starting at _first[k], sorted by
    // increasing _byte (the char leading from the parent to that node).
    std::vector<uint32_t> _first;
    std::vector<unsigned char> _nchild;
    std::vector<unsigned char> _byte;
    std::vector<unsigned char> _term; // per node: 0, or a word ends here (WORD or WEAK)
    uint32_t _root_child[TWB_NB_BYTES]; // children of the root, by char (0 if none)
    std::vector<uint32_t> _root_grandchild; // [c1*TWB_NB_BYTES+c2]: node for the 2 chars c1c2 (0 if none)
    size_t _nb_words;
    enum { WORD = 1, WEAK = 2 };

    // Find the child of node by char c, or 0 if none
    inline uint32_t child(uint32_t node, unsigned char c) const {
        const unsigned char * bytes = _byte.data();
        uint32_t k = _first[node];
        uint32_t end = k + _nchild[node];
        if ( end - k > 8 ) { // binary search the first >= c
            while ( k < end ) {
                uint32_t m = k + (end-k)/2;
                if ( bytes[m] < c ) k = m+1; else end = m;
            }
            return k < _first[node] + _nchild[node] && bytes[k] == c ? k : 0;
        }
        for ( ; k < end; k++ ) {
            if ( bytes[k] >= c )
                return bytes[k] == c ? k : 0;
        }
        return 0;
    }

    // Find the node reached by the word w (byte encoded), or 0 if none
    uint32_t find(const std::string & w) const {
        if ( w.empty() || _nb_words == 0 )
            return 0;
        unsigned char b = (unsigned char)w[0];
        uint32_t node = b < TWB_NB_BYTES ? _root_child[b] : 0;
        for ( size_t d=1; node && d<w.size(); d++ )
            node = child(node, (unsigned char)w[d]);
        return node;
    }

    // Add the children of 'node', made from the words [lo, hi) (sorted)
    // that all share their first d bytes, and are all longer than that.
    void addChildren(const std::vector<std::string> & words, size_t lo, size_t hi, size_t d, uint32_t node) {
        // Create the children nodes: one per distinct byte at index d
        // (words being sorted, the ones sharing it are contiguous)
        uint32_t first = (uint32_t)_term.size();
        for ( size_t i=lo; i<hi; ) {
            unsigned char c = (unsigned char)words[i][d];
            _byte.push_back(c);
            _term.push_back(0);
            _first.push_back(0);
            _nchild.push_back(0);
            while ( i < hi && (unsigned char)words[i][d] == c )
                i++;
        }
        _first[node] = first;
        _nchild[node] = (unsigned char)(_term.size() - first); // (at most TWB_NB_BYTES)
        // Then their own children
        uint32_t ch = first;
        for ( size_t i=lo; i<hi; ch++ ) {
            unsigned char c = (unsigned char)words[i][d];
            size_t j = i;
            while ( j < hi && (unsigned char)words[j][d] == c )
                j++;
            // words[i..j) share d+1 bytes: if one is exactly that long,
            // it is the first one, and it ends at this child.
            if ( words[i].size() == d+1 ) {
                _term[ch] = WORD;
                i++;
            }
            if ( i < j )
                addChildren(words, i, j, d+1, ch);
            i = j;
        }
    }

public:
    Dict() { clear(); }

    void clear() {
        std::vector<uint32_t>().swap(_first);
        std::vector<unsigned char>().swap(_nchild);
        std::vector<unsigned char>().swap(_byte);
        std::vector<unsigned char>().swap(_term);
        memset(_root_child, 0, sizeof(_root_child));
        std::vector<uint32_t>().swap(_root_grandchild);
        _nb_words = 0;
    }

    size_t size() const { return _nb_words; }

    // Build from a list of byte encoded words (which will be sorted in place)
    void build(std::vector<std::string> & words) {
        clear();
        std::sort(words.begin(), words.end());
        words.erase(std::unique(words.begin(), words.end()), words.end());
        if ( !words.empty() && words[0].empty() ) // (not expected)
            words.erase(words.begin());
        if ( words.empty() )
            return;
        _nb_words = words.size();
        // Count the nodes, to allocate them all at once: a node per
        // distinct prefix, i.e. per char not shared with the previous word.
        size_t nb_nodes = 1;
        for ( size_t i=0; i<words.size(); i++ ) {
            size_t common = 0;
            if ( i > 0 ) {
                const std::string & p = words[i-1];
                const std::string & w = words[i];
                while ( common < p.size() && common < w.size() && p[common] == w[common] )
                    common++;
            }
            nb_nodes += words[i].size() - common;
        }
        _first.reserve(nb_nodes);
        _nchild.reserve(nb_nodes);
        _byte.reserve(nb_nodes);
        _term.reserve(nb_nodes);
        _byte.push_back(0); // root
        _term.push_back(0);
        _first.push_back(0);
        _nchild.push_back(0);
        addChildren(words, 0, words.size(), 0, 0);
        for ( uint32_t k=_first[0]; k<_first[0]+_nchild[0]; k++ ) {
            if ( _byte[k] < TWB_NB_BYTES )
                _root_child[_byte[k]] = k;
        }
        _root_grandchild.assign(TWB_NB_BYTES*TWB_NB_BYTES, 0);
        for ( unsigned c1=0; c1<TWB_NB_BYTES; c1++ ) {
            uint32_t n1 = _root_child[c1];
            if ( !n1 )
                continue;
            for ( uint32_t k=_first[n1]; k<_first[n1]+_nchild[n1]; k++ ) {
                if ( _byte[k] < TWB_NB_BYTES )
                    _root_grandchild[c1*TWB_NB_BYTES + _byte[k]] = k;
            }
        }
    }

    // Flag as weak the words (byte encoded) of the provided list that are
    // in this dict. Returns the number of words flagged.
    int markWeak(const std::vector<std::string> & words) {
        int nb = 0;
        for ( size_t i=0; i<words.size(); i++ ) {
            uint32_t node = find(words[i]);
            if ( node && _term[node] == WORD ) {
                _term[node] = WEAK;
                nb++;
            }
        }
        return nb;
    }

    // Find all the words that are a prefix of s[0..n), and call f(len, weak)
    // for each of them, by increasing length.
    template <typename F>
    void forEachPrefix(const unsigned char * s, int n, F f) const {
        if ( n <= 0 || _nb_words == 0 )
            return;
        unsigned char b = s[0];
        if ( b >= TWB_NB_BYTES )
            return;
        uint32_t node = _root_child[b];
        if ( !node )
            return;
        const unsigned char * term = _term.data();
        if ( term[node] )
            f(1, term[node] == WEAK);
        if ( n < 2 || s[1] >= TWB_NB_BYTES )
            return;
        node = _root_grandchild[b*TWB_NB_BYTES + s[1]];
        for ( int d=2; node; d++ ) {
            if ( term[node] )
                f(d, term[node] == WEAK);
            if ( d >= n )
                return;
            node = child(node, s[d]);
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

// Costs are (unknown chars, words), compared lexicographically:
// packed into an int64 so they can be added and compared directly.
const int64_t COST_INF = (int64_t)1 << 60;

struct Edge {
    int from;
    int to;
    int64_t cost;
    bool known;
    bool weak; // a known but weak word: no break on either side of it
    bool best; // on some best path
};

// Per node (boundary between s[k-1] and s[k], for k in 0..n)
struct Node {
    int64_t fwd;         // best cost from 0 to node
    int64_t bwd;         // best cost from node to end
    int cross;           // difference array: best edges strictly crossing node
    int stamp;           // to dedup matches from both dicts
    unsigned char bnd;   // cluster boundary
    unsigned char in_unk;    // some best edge ending at node is unknown
    unsigned char out_unk;   // some best edge starting at node is unknown
    unsigned char left_bad;  // some best edge ending at node starts at a node with in_unk
    unsigned char right_bad; // some best edge starting at node ends at a node with out_unk
    unsigned char weak_adj;  // some best edge ending or starting at node is a weak word
};

// Reused buffers
std::vector<unsigned char> _s;   // normalized run
std::vector<int> _map;           // normalized index => run index
std::vector<Edge> _edges;
std::vector<Node> _nodes;
std::vector<int> _breaks;        // breaks found in a run

inline Edge makeEdge(int from, int to, bool known, bool weak) {
    Edge e;
    e.from = from;
    e.to = to;
    e.cost = known ? 1 : ((int64_t)(to - from) << 32);
    e.known = known;
    e.weak = weak;
    e.best = false;
    return e;
}

// Segment the normalized run s[0..n), and push in 'breaks' the
// normalized indexes k (1 <= k < n) where a break is allowed
// between s[k-1] and s[k].
void segmentRun(const unsigned char * s, int n, std::vector<int> & breaks) {
    if ( n < 2 )
        return;
    Node init;
    init.fwd = COST_INF;
    init.bwd = COST_INF;
    init.cross = 0;
    init.stamp = -1;
    init.bnd = 1;
    init.in_unk = init.out_unk = init.left_bad = init.right_bad = init.weak_adj = 0;
    _nodes.assign(n+2, init); // (n+1 for 'cross')
    Node * nodes = _nodes.data();
    for ( int k=1; k<n; k++ ) {
        if ( isFollowingByte(s[k]) || isLeadingByte(s[k-1]) )
            nodes[k].bnd = 0;
    }
    // Build edges, grouped by increasing 'from'
    _edges.clear();
    for ( int i=0; i<n; i++ ) {
        if ( !nodes[i].bnd )
            continue;
        auto addKnown = [&](int len, bool weak) {
            int j = i + len;
            if ( nodes[j].bnd && nodes[j].stamp != i ) {
                nodes[j].stamp = i;
                _edges.push_back(makeEdge(i, j, true, weak));
            }
        };
        // User words first: if also in the main dict, they are never weak
        _user_dict.forEachPrefix(s+i, n-i, addKnown);
        _main_dict.forEachPrefix(s+i, n-i, addKnown);
        if ( isWordMarkByte(s[i]) && nodes[i+1].bnd && nodes[i+1].stamp != i ) {
            // MAI YAMOK and PAIYANNOI are known words on their own
            nodes[i+1].stamp = i;
            _edges.push_back(makeEdge(i, i+1, true, false));
        }
        // Unknown text up to the next cluster boundary
        int j = i + 1;
        while ( !nodes[j].bnd )
            j++;
        _edges.push_back(makeEdge(i, j, false, false));
    }
    Edge * edges = _edges.data();
    int nb_edges = (int)_edges.size();
    // Forward best costs (edges are sorted by 'from', and from < to)
    nodes[0].fwd = 0;
    for ( int e=0; e<nb_edges; e++ ) {
        const Edge & ed = edges[e];
        int64_t f = nodes[ed.from].fwd;
        if ( f >= COST_INF )
            continue;
        int64_t c = f + ed.cost;
        if ( c < nodes[ed.to].fwd )
            nodes[ed.to].fwd = c;
    }
    // Backward best costs
    nodes[n].bwd = 0;
    for ( int e=nb_edges-1; e>=0; e-- ) {
        const Edge & ed = edges[e];
        int64_t b = nodes[ed.to].bwd;
        if ( b >= COST_INF )
            continue;
        int64_t c = b + ed.cost;
        if ( c < nodes[ed.from].bwd )
            nodes[ed.from].bwd = c;
    }
    const int64_t best = nodes[n].fwd;
    // Look at best edges (those on some best path)
    for ( int e=0; e<nb_edges; e++ ) {
        Edge & ed = edges[e];
        if ( nodes[ed.from].fwd >= COST_INF || nodes[ed.to].bwd >= COST_INF )
            continue;
        if ( nodes[ed.from].fwd + ed.cost + nodes[ed.to].bwd != best )
            continue;
        ed.best = true;
        if ( ed.to - ed.from > 1 ) { // nodes from+1 .. to-1 are jumped over
            nodes[ed.from+1].cross++;
            nodes[ed.to].cross--;
        }
        if ( !ed.known ) {
            nodes[ed.from].out_unk = 1;
            nodes[ed.to].in_unk = 1;
        }
    }
    for ( int e=0; e<nb_edges; e++ ) {
        const Edge & ed = edges[e];
        if ( !ed.best )
            continue;
        if ( ed.weak ) {
            nodes[ed.from].weak_adj = 1;
            nodes[ed.to].weak_adj = 1;
        }
        if ( nodes[ed.from].in_unk )
            nodes[ed.to].left_bad = 1;
        if ( nodes[ed.to].out_unk )
            nodes[ed.from].right_bad = 1;
    }
    int crossing = 0;
    for ( int k=1; k<n; k++ ) {
        const Node & nd = nodes[k];
        crossing += nd.cross;
        if ( !nd.bnd || isWordMarkByte(s[k]) )
            continue;
        if ( crossing > 0 ) // some best path does not go through k
            continue;
        if ( nd.fwd + nd.bwd != best ) // (should not happen if not crossed)
            continue;
        if ( nd.in_unk || nd.out_unk ) // unknown text on one side
            continue;
        if ( nd.left_bad || nd.right_bad ) // known word next to unknown text
            continue;
        if ( nd.weak_adj ) // weak word on one side
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
    std::vector<int> & breaks = _breaks;
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
    std::vector<Edge>().swap(_edges);
    std::vector<Node>().swap(_nodes);
    std::vector<int>().swap(_breaks);
}

}
