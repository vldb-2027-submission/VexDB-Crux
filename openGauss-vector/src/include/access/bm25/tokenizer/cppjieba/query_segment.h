/**
 * Copyright ...
 */

#ifndef CPPJIEBA_QUERYSEGMENT_H
#define CPPJIEBA_QUERYSEGMENT_H

#include <algorithm>

#include "access/bm25/tokenizer/cppjieba/dict_trie.h"
#include "access/bm25/tokenizer/cppjieba/segment_base.h"
#include "access/bm25/tokenizer/cppjieba/full_segment.h"
#include "access/bm25/tokenizer/cppjieba/mix_segment.h"
#include "access/bm25/tokenizer/cppjieba/unicode.h"

namespace cppjieba {
class QuerySegment : public SegmentBase {
public:
    QuerySegment(const DictTrie* dictTrie, const HMMModel* model) : mixSeg_(dictTrie, model), trie_(dictTrie) {}
    void destroy() { SegmentBase::destroy(); }
    virtual void Cut(RuneStrArray::const_iterator begin, RuneStrArray::const_iterator end,
                     Vector<WordRange>& res) const override
    {
        //use mix Cut first
        Vector<WordRange> mixRes;
        mixSeg_.CutRuneArray(begin, end, mixRes);
        Vector<WordRange> fullRes;
        for (Vector<WordRange>::const_iterator mixResItr = mixRes.begin(); mixResItr != mixRes.end(); mixResItr++) {
            if (mixResItr->Length() > 2) {
                for (size_t i = 0; i + 1 < mixResItr->Length(); i++) {
                    char *text = EncodeRunesToString(mixResItr->left + i, mixResItr->left + i + 2);
                    if (trie_->Find(text) != NULL) {
                        WordRange wr(mixResItr->left + i, mixResItr->left + i + 1);
                        res.push_back(wr);
                    }
                }
            }
            if (mixResItr->Length() > 3) {
                for (size_t i = 0; i + 2 < mixResItr->Length(); i++) {
                    char *text = EncodeRunesToString(mixResItr->left + i, mixResItr->left + i + 3);
                    if (trie_->Find(text) != NULL) {
                        WordRange wr(mixResItr->left + i, mixResItr->left + i + 2);
                        res.push_back(wr);
                    }
                }
            }
            res.push_back(*mixResItr);
        }
        ann_helper::optional_destroy(mixRes);
        ann_helper::optional_destroy(fullRes);
    }

private:
    bool IsAllAscii(const Unicode &s) const
    {
        for (size_t i = 0; i < s.size(); i++) {
            if (s[i] >= 0x80) {
                return false;
            }
        }
        return true;
    }
    MixSegment mixSeg_;
    const DictTrie *trie_;
};

}  /* namespace cppjieba */

#endif /* CPPJIEBA_QUERYSEGMENT_H */
