/** \file thaiwordbreak.h
    \brief Dictionary based line breaking opportunities for Thai text

    Thai is written without spaces between words, and libunibreak (UAX#14)
    resolves Thai letters (class SA) to AL, so it provides no break
    opportunity inside a run of Thai text. This module finds word
    boundaries in such runs using a word list, so we can wrap lines
    between words instead of having a whole Thai phrase be unbreakable
    (or broken at an arbitrary character when it doesn't fit).

    Design goal: never allow a break in the middle of a word. So it is
    conservative: a break is only allowed between two dictionary words,
    and only when all the best (fewest unknown chars, then fewest words)
    segmentations agree on it. Text not in the dictionary gets no break,
    and neither does the side of a "weak" word (see setMainDictPath()).

    This file has no dependency on crengine types, so it can be reused
    by other text layout code (and built standalone for testing).

    CoolReader Engine / KOReader
    This source code is distributed under the terms of
    GNU General Public License
    See LICENSE file for details
*/

#ifndef __THAI_WORD_BREAK_H_INCLUDED__
#define __THAI_WORD_BREAK_H_INCLUDED__

#include <stdint.h>
#include <stddef.h>

namespace ThaiWordBreak {

/// Callback called for each allowed break: a line break is allowed after text[pos].
typedef void (*break_cb_t)(int pos, void * ctx);

/// Set the path of the main word list (UTF-8, one word per line), and
/// optionally of a list of "weak" words (same format): words of the main
/// list that are rare and short, often met as syllables of names and
/// transliterated foreign words (ร่า, นิ, กี้...). They are still used to
/// segment the text, but we never allow a break on either side of them.
/// Both are loaded lazily, the first time some Thai text needs them.
/// An empty path (or NULL) disables Thai word breaking.
void setMainDictPath(const char * path, const char * weak_path = NULL);

/// Load the user word list (same format, additional words to the main one).
/// It is loaded immediately (it's expected to be small).
/// An empty path (or NULL) clears it.
/// Returns the number of words loaded, or -1 if the file could not be read.
int setUserDict(const char * path);

/// Add words from a buffer (UTF-8, one word per line) to the user word list,
/// replacing any previous user words. Returns the number of words loaded.
int setUserDictFromBuffer(const char * buf, size_t size);

/// Hash of the current configuration (main dict path and user dict
/// content), to be included in rendering hashes so that a change
/// triggers a re-rendering. Does not depend on lazy loading state.
uint32_t getHash();

/// Whether Thai word breaking is configured (a main dict path is set).
bool isEnabled();

/// Whether c is a Thai char that is part of a run we segment
/// (letters, vowels, tone marks, but not digits and punctuation).
inline bool isThaiChar(uint32_t c) {
    return (c >= 0x0E01 && c <= 0x0E3A) || (c >= 0x0E40 && c <= 0x0E4E);
}

/// Whether text contains any Thai char.
bool hasThai(const char32_t * text, int len);

/// Find the allowed line breaks inside the Thai runs of text[0..len).
/// For each one, callback(pos, ctx) is called with pos the index of
/// the char a break is allowed after (both text[pos] and text[pos+1]
/// are Thai chars). Breaks at the edges of Thai runs are not reported
/// (those are handled by the generic line breaking rules).
/// If fallback_callback is provided, it is called (same arguments) for each
/// "fallback" break: a boundary between two dictionary words (that all the
/// best segmentations agree on) that we don't trust enough to make it a
/// normal break opportunity, because some text not in the dictionary is
/// nearby. They are meant to be used only when no normal break would fill
/// a line well enough (like hyphenation). We never report a break next to
/// text not in the dictionary or a weak word, nor inside a cluster.
/// Returns the number of (normal) breaks reported.
int findBreaks(const char32_t * text, int len, break_cb_t callback, void * ctx,
               break_cb_t fallback_callback = NULL);

/// Free the main word list and work buffers (the configuration is kept:
/// the main word list will be loaded again if needed, the user words stay).
void release();

}

#endif
