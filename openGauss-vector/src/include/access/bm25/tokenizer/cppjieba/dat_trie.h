/**
 * Copyright ...
 */

#ifndef CPPJIEBA_DATTRIE_HPP
#define CPPJIEBA_DATTRIE_HPP

#include <algorithm>
#include <utility>
#include <vtl/vector>
#include <vtl/pair>

#include "access/bm25/tokenizer/cppjieba/unicode.h"
#include "access/bm25/tokenizer/cppjieba/darts.h"

namespace cppjieba {

struct DatElement {
    const char *word;
    double weight{0.0};

    bool operator<(const DatElement &b) const
    {
        int cmp = strcmp(word, b.word);
        if (cmp == 0) {
            return weight > b.weight;
        }
        return cmp < 0; 
    }
};

struct DatDag {
    Vector<Pair<size_t, const DatElement *>> nexts;
    double max_weight;
    int max_next;

    void destroy() { ann_helper::optional_destroy(nexts); }
};

typedef Darts::DoubleArray JiebaDAT;

class DatTrie {
public:
    DatTrie(Vector<DatElement> &static_node) : static_node_(static_node) {}
    void destroy() { dat_.destroy(); }

    const DatElement *Find(const char *key) const
    {
        JiebaDAT::result_pair_type find_result;
        dat_.exactMatchSearch(key, find_result);
        if ((0 == find_result.length) || (find_result.value < 0) 
                                      || (find_result.value >= (int)elements_num_)) {
            return NULL;
        }
        return static_node_.at(find_result.value);
    }

    void Find(RuneStrArray::const_iterator begin, RuneStrArray::const_iterator end,
              Vector<DatDag> &res) const
    {
        res.clear();
        res.resize(end - begin);
        char *text_str = EncodeRunesToString(begin, end);

        for (size_t i = 0, begin_pos = 0; i < size_t(end - begin); ++i) {
            constexpr size_t max_num = 128ul;
            JiebaDAT::result_pair_type result_pairs[max_num] = {};
            size_t num_results = dat_.commonPrefixSearch(&text_str[begin_pos], &result_pairs[0], max_num);

            res[i].nexts.emplace_back(i + 1, nullptr);

            for (size_t idx = 0; idx < num_results; ++idx) {
                auto &match = result_pairs[idx];
                if ((match.value < 0) || (match.value >= (int)elements_num_)) {
                    continue;
                }
                const size_t char_num = Utf8CharNum(&text_str[begin_pos], match.length);
                DatElement *pvalue = &static_node_[match.value];
                if (char_num == 1ul) {
                    res[i].nexts[0].second = pvalue;
                    continue;
                }
                res[i].nexts.emplace_back(i + char_num, pvalue);
            }
            begin_pos += UnicodeToUtf8Bytes((begin + i)->rune);
        }
        pfree(text_str);
    }

    double GetMinWeight() const { return min_weight_; }

    void SetMinWeight(double d) { min_weight_ = d; }

    void InitBuildDat(Vector<DatElement> &elements)
    {
        std::sort(elements.begin(), elements.end());

        elements_num_ = elements.size();
        Vector<const char *> keys_ptr_vec(elements_num_);
        Vector<int> values_vec(elements_num_);

        for (size_t i = 0; i < elements_num_; ++i) {
            keys_ptr_vec.push_back(elements[i].word);
            values_vec.push_back(i);
        }

        dat_.build(elements_num_, keys_ptr_vec.data(), NULL, values_vec.data());
        ann_helper::optional_destroy(keys_ptr_vec);
        ann_helper::optional_destroy(values_vec);
    }

private:
    JiebaDAT dat_;
    Vector<DatElement> &static_node_;
    size_t elements_num_{0};
    double min_weight_{0.0};
};

}  /* namespace cppjieba */
#endif /* CPPJIEBA_DATTRIE_HPP */
