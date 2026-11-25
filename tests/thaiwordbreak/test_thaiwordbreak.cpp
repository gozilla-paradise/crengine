// Standalone test for crengine/src/thaiwordbreak.cpp (no other dependency).
//
// Build and run, from the crengine repository root:
//   c++ -std=c++11 -O2 -Icrengine/src -o /tmp/test_thaiwordbreak tests/thaiwordbreak/test_thaiwordbreak.cpp
//   /tmp/test_thaiwordbreak cr3gui/data/wordbreak/th_words.txt cr3gui/data/wordbreak/th_weak_words.txt \
//       tests/thaiwordbreak/sentences.txt tests/thaiwordbreak/user_words.txt
//
// sentences.txt: one sentence per line, '|' marks the places where a line
// break is acceptable (word boundaries). Lines starting with '#' are comments.
// Each sentence is printed with the breaks we found: '|' if acceptable,
// '!' if not (a break in the middle of a word). Exit status is 1 if any '!'.
// Fallback breaks (only used when a line would be left too empty) are shown
// as '/' if acceptable, '?' if not (these don't affect the exit status).

#include "thaiwordbreak.cpp"
#include <set>
#include <string>

static std::u32string decode(const std::string & s) {
    std::u32string out;
    size_t i = 0;
    while ( i < s.size() ) {
        uint32_t cp;
        int len = ThaiWordBreak::decodeUtf8((const unsigned char *)s.data() + i, s.size() - i, &cp);
        if ( len == 0 ) { i++; continue; }
        out.push_back((char32_t)cp);
        i += len;
    }
    return out;
}

static std::string encode(const std::u32string & s) {
    std::string out;
    for ( char32_t c : s ) {
        uint32_t v = c;
        if ( v < 0x80 ) out += (char)v;
        else if ( v < 0x800 ) { out += (char)(0xC0|(v>>6)); out += (char)(0x80|(v&0x3F)); }
        else if ( v < 0x10000 ) { out += (char)(0xE0|(v>>12)); out += (char)(0x80|((v>>6)&0x3F)); out += (char)(0x80|(v&0x3F)); }
        else { out += (char)(0xF0|(v>>18)); out += (char)(0x80|((v>>12)&0x3F)); out += (char)(0x80|((v>>6)&0x3F)); out += (char)(0x80|(v&0x3F)); }
    }
    return out;
}

static void addBreak(int pos, void * ctx) {
    ((std::set<int> *)ctx)->insert(pos);
}

static std::set<int> _fallback_found;
static void addFallbackBreak(int pos, void * ctx) {
    _fallback_found.insert(pos);
}

int main(int argc, char ** argv) {
    if ( argc < 4 ) {
        fprintf(stderr, "usage: %s th_words.txt th_weak_words.txt sentences.txt [user_words.txt]\n", argv[0]);
        return 2;
    }
    ThaiWordBreak::setMainDictPath(argv[1], argv[2]);
    if ( argc > 4 )
        printf("user words: %d\n", ThaiWordBreak::setUserDict(argv[4]));
    FILE * f = fopen(argv[3], "rb");
    if ( !f ) {
        fprintf(stderr, "cannot open %s\n", argv[3]);
        return 2;
    }
    char line[8192];
    int nb_good = 0, nb_bad = 0, nb_missed = 0, nb_fb_good = 0, nb_fb_bad = 0;
    while ( fgets(line, sizeof(line), f) ) {
        std::string l(line);
        while ( !l.empty() && (l.back() == '\n' || l.back() == '\r') )
            l.pop_back();
        if ( l.empty() || l[0] == '#' )
            continue;
        std::u32string marked = decode(l), text;
        std::set<int> ok, found;
        for ( char32_t c : marked ) {
            if ( c == U'|' ) {
                if ( !text.empty() ) ok.insert((int)text.size() - 1);
            }
            else {
                text.push_back(c);
            }
        }
        _fallback_found.clear();
        ThaiWordBreak::findBreaks(text.data(), (int)text.size(), addBreak, &found, addFallbackBreak);
        std::u32string out;
        for ( int i=0; i<(int)text.size(); i++ ) {
            out.push_back(text[i]);
            if ( found.count(i) ) {
                out.push_back(ok.count(i) ? U'|' : U'!');
                if ( ok.count(i) ) nb_good++; else nb_bad++;
            }
            else if ( _fallback_found.count(i) ) {
                out.push_back(ok.count(i) ? U'/' : U'?');
                if ( ok.count(i) ) nb_fb_good++; else nb_fb_bad++;
            }
            else if ( ok.count(i) && i+1 < (int)text.size()
                        && ThaiWordBreak::isThaiChar(text[i]) && ThaiWordBreak::isThaiChar(text[i+1]) ) {
                nb_missed++;
            }
        }
        printf("%s\n", encode(out).c_str());
    }
    fclose(f);
    printf("\nbreaks: good=%d BAD=%d not found=%d\n", nb_good, nb_bad, nb_missed);
    printf("fallback breaks: good=%d bad=%d\n", nb_fb_good, nb_fb_bad);
    return nb_bad > 0 ? 1 : 0;
}
