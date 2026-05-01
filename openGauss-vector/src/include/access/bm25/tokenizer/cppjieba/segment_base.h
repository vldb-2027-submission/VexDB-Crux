/**
 * Copyright ...
 */

#ifndef CPPJIEBA_SEGMENTBASE_H
#define CPPJIEBA_SEGMENTBASE_H

#include "access/bm25/tokenizer/cppjieba/pre_filter.h"

namespace cppjieba {
constexpr char *SPECIAL_SEPARATORS = "\t\n\xEF\xBC\x8C\xE3\x80\x82";

class SegmentBase {
public:
    SegmentBase() { ResetSeparators(SPECIAL_SEPARATORS); }

    virtual void Cut(RuneStrArray::const_iterator begin, RuneStrArray::const_iterator end,
                     Vector<WordRange>& res) const = 0;

    void CutToWord(const char *sentence, Vector<Word> &words) const
    {
        PreFilter pre_filter(symbols_, sentence);
        Vector<WordRange> wrs;
        wrs.reserve(strlen(sentence) / 2);

        while (pre_filter.HasNext()) {
            auto range = pre_filter.Next();
            Cut(range.begin, range.end, wrs);
        }

        words.clear();
        words.reserve(wrs.size());
        GetWordsFromWordRanges(sentence, wrs, words);
        ann_helper::optional_destroy(pre_filter);
        ann_helper::optional_destroy(wrs);
    }

    void CutRuneArray(RuneStrArray::const_iterator begin, RuneStrArray::const_iterator end,
                      Vector<WordRange>& res) const
    {
        Cut(begin, end, res);
    }

    void ResetSeparators(const char *s)
    {
        symbols_.clear();
        RuneStrArray runes;
        DecodeUTF8RunesInString(s, runes);
        for (size_t i = 0; i < runes.size(); ++i) {
            symbols_.insert(runes[i].rune);
        }
        ann_helper::optional_destroy(runes);
    }

    void destroy() { ann_helper::optional_destroy(symbols_); }
protected:
    UnorderedSet<Rune> symbols_;
};
}  /* namespace cppjieba */

#endif /* CPPJIEBA_SEGMENTBASE_H */
