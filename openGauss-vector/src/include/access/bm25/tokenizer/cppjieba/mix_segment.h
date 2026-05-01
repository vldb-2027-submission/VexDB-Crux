/**
 * Copyright ...
 */

#ifndef CPPJIEBA_MIXSEGMENT_H
#define CPPJIEBA_MIXSEGMENT_H

#include <cassert>
#include "access/bm25/tokenizer/cppjieba/mp_segment.h"
#include "access/bm25/tokenizer/cppjieba/hmm_segment.h"
#include "access/bm25/tokenizer/cppjieba/string_util.h"
namespace cppjieba {
class MixSegment : public SegmentBase {
public:
    MixSegment(const DictTrie *dictTrie, const HMMModel *model) : mpSeg_(dictTrie), hmmSeg_(model) {}
    void destroy() { SegmentBase::destroy(); }
    virtual void Cut(RuneStrArray::const_iterator begin,
                     RuneStrArray::const_iterator end, 
                     Vector<WordRange>& res) const override
    {
        Vector<WordRange> words;
        Assert(end >= begin);
        words.reserve(end - begin);
        mpSeg_.CutRuneArray(begin, end, words);

        Vector<WordRange> hmmRes;
        hmmRes.reserve(end - begin);

        for (size_t i = 0; i < words.size(); ++i) {
            if (words[i].left != words[i].right || 
                (words[i].left == words[i].right && mpSeg_.IsUserDictSingleChineseWord(words[i].left->rune))) {
                res.emplace_back(words[i]);
                continue;
            }
            size_t j = i;
            while (j < words.size() && words[j].left == words[j].right &&
                   !mpSeg_.IsUserDictSingleChineseWord(words[j].left->rune)) {
                j++;
            }
            Assert(j - 1 >= i);
            hmmSeg_.CutRuneArray(words[i].left, words[j - 1].left + 1, hmmRes);

            for (size_t k = 0; k < hmmRes.size(); k++) {
                res.push_back(hmmRes[k]);
            }
            hmmRes.clear();
            i = j - 1;
        }
        ann_helper::optional_destroy(words);
        ann_helper::optional_destroy(hmmRes);
    }

private:
    MPSegment mpSeg_;
    HMMSegment hmmSeg_;
};

}  /* namespace cppjieba */

#endif  /* CPPJIEBA_MIXSEGMENT_H */
