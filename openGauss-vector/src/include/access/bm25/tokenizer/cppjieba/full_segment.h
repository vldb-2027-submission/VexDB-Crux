/**
 * Copyright ...
 */

#ifndef CPPJIEBA_FULLSEGMENT_H
#define CPPJIEBA_FULLSEGMENT_H

#include <algorithm>
#include <vtl/vector>
#include "access/bm25/tokenizer/cppjieba/dict_trie.h"
#include "access/bm25/tokenizer/cppjieba/pre_filter.h"

namespace cppjieba {
class FullSegment : public SegmentBase {
public:
    FullSegment(const DictTrie *dictTrie) : dictTrie_(dictTrie) {}
    void destroy() { SegmentBase::destroy(); }
    virtual void Cut(RuneStrArray::const_iterator begin, RuneStrArray::const_iterator end,
                     Vector<WordRange>& res) const override
    {
        Assert(dictTrie_);
        Vector<DatDag> dags;
        dictTrie_->Find(begin, end, dags);
        size_t max_word_end_pos = 0;

        for (size_t i = 0; i < dags.size(); ++i) {
            for (const auto &kv : dags[i].nexts) {
                const size_t nextoffset = kv.first - 1;
                Assert(nextoffset < dags.size());
                const auto wordLen = nextoffset - i + 1;
                const bool is_not_covered_single_word = ((dags[i].nexts.size() == 1) && (max_word_end_pos <= i));
                const bool is_oov = (NULL == kv.second); //Out-of-Vocabulary

                if ((is_not_covered_single_word) || ((!is_oov) && (wordLen >= 2))) {
                    WordRange wr(begin + i, begin + nextoffset);
                    res.push_back(wr);
                }
                max_word_end_pos = std::max(max_word_end_pos, nextoffset + 1);
            }
        }
        ann_helper::optional_destroy(dags);
    }

private:
    const DictTrie *dictTrie_;
};
}  /* namespace cppjieba */

#endif /* CPPJIEBA_FULLSEGMENT_H */
