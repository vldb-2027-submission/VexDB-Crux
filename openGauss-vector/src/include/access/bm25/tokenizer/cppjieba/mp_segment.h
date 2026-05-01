/**
 * Copyright ...
 */

#ifndef CPPJIEBA_MPSEGMENT_H
#define CPPJIEBA_MPSEGMENT_H

#include <algorithm>
#include <cassert>
#include "access/bm25/tokenizer/cppjieba/dict_trie.h"

namespace cppjieba {

class MPSegment : public SegmentBase {
public:
    MPSegment(const DictTrie *dictTrie) : dictTrie_(dictTrie) {}
    void destroy() { SegmentBase::destroy(); }
    void Cut(RuneStrArray::const_iterator begin, RuneStrArray::const_iterator end,
             Vector<WordRange>& words) const override
    {
        Vector<DatDag> dags;
        dictTrie_->Find(begin, end, dags);
        CalcDP(dags);
        CutByDag(begin, end, dags, words);
        ann_helper::optional_destroy(dags);
    }

    bool IsUserDictSingleChineseWord(const Rune &value) const
    {
        return dictTrie_->IsUserDictSingleChineseWord(value);
    }

private:
    void CalcDP(Vector<DatDag> &dags) const 
    {
        for (auto rit = dags.rbegin(); rit != dags.rend(); --rit) {
            rit->max_next = -1;
            rit->max_weight = MIN_DOUBLE;

            for (const auto &it : rit->nexts) {
                const auto nextPos = it.first;
                double val = dictTrie_->GetMinWeight();

                if (it.second) {
                    val = it.second->weight;
                }

                if (nextPos < dags.size()) {
                    val += dags[nextPos].max_weight;
                }

                if ((nextPos <= dags.size()) && (val > rit->max_weight)) {
                    rit->max_weight = val;
                    rit->max_next = nextPos;
                }
            }
        }
    }

    void CutByDag(RuneStrArray::const_iterator begin, RuneStrArray::const_iterator,
                  const Vector<DatDag> &dags, Vector<WordRange> &words) const
    {
        for (size_t i = 0; i < dags.size();) {
            const auto next = dags[i].max_next;
            Assert((size_t)next > i);
            Assert((size_t)next <= dags.size());
            WordRange wr(begin + i, begin + next - 1);
            words.push_back(wr);
            i = next;
        }
    }

    const DictTrie *dictTrie_;
};

}  /* namespace cppjieba */

#endif /* CPPJIEBA_MPSEGMENT_H */
