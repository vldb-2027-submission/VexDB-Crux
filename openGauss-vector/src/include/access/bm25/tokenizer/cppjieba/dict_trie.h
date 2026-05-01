/**
 * Copyright ...
 */

#ifndef CPPJIEBA_DICT_TRIE_HPP
#define CPPJIEBA_DICT_TRIE_HPP

#include <fstream>      /* ifstream */
#include <algorithm>    /* min */

#include <vtl/vector>
#include <vtl/hashtable>

#include "access/bm25/tokenizer/cppjieba/string_util.h"
#include "access/bm25/tokenizer/cppjieba/dat_trie.h"
#include "access/bm25/tokenizer/token_pool.h"
#include "utils/palloc.h"
#include "catalog/pg_ts_content.h"

namespace cppjieba {

const double MIN_DOUBLE = -3.14e+100;
const double MAX_DOUBLE = 3.14e+100;

/* in default dict, the sum is 60101984 and the biggest weight is 883634 */
constexpr double DEFAULT_WEIGHT = 1e4; /* bigger than most of weight in default dict */

typedef uint32 Rune;

class DictTrie : public BaseObject {
public:
    DictTrie(const char *dict_path, ArrayCStr &userdict_arr,
             bool case_sensitive, bm25_tokenizer::TokenPool *pool)
    : case_sensitive_(case_sensitive), pool_(pool)
    {
        if (dict_path) {
            LoadDict(dict_path);
        }
        LoadUserDict(userdict_arr);
        freq_sum_ = CalcFreqSum(static_node_infos_);
        CalculateWeight(static_node_infos_, freq_sum_);
        double min_weight = DBL_MAX;
        SetStaticWordWeights(min_weight);
        dat_trie_.SetMinWeight(min_weight);
        static_node_infos_.shrink_to_fit();
        dat_trie_.InitBuildDat(static_node_infos_);
    }

    void LoadUserDict(ArrayCStr &userdict_arr)
    {
        for (int32 i = 0; i < userdict_arr.count; ++i) {
            char *line = userdict_arr.values[i];

            constexpr size_t userdict_args_num = 2ul;
            char *tokens[userdict_args_num] = {NULL};
            size_t args_num = split(tokens, line, keyword_sep, userdict_args_num);
    
            constexpr size_t slice_word_num_max = 10ul; /* impossible to be bigger? */
            char *words[slice_word_num_max] = {NULL};
            size_t slice_word_num = split(words, tokens[0], slice_sep, slice_word_num_max);

            for (size_t j = 0; j < slice_word_num; ++j) {
                DatElement node_info;
                if (!case_sensitive_) {
                    words[j] = pg_strtolower(words[j]);
                }
                node_info.word = pool_->get_token(words[j]);

                if (args_num == 1) {
                    node_info.weight = DEFAULT_WEIGHT;
                } else if (args_num == 2) {
                    const int freq = atoi(tokens[1]);
                    node_info.weight = freq;
                }
                static_node_infos_.push_back(node_info);
    
                if (Utf8CharNum(node_info.word, strlen(node_info.word)) == 1) {
                    RuneArray word;
                    if (DecodeRunesInString(node_info.word, word)) {
                        user_dict_single_chinese_word_.insert(word[0]);
                    } else {
                        ereport(ERROR,
                            (errcode(ERRCODE_SYNTAX_ERROR),
                             errmsg("Decode \"%s\" failed", node_info.word)));
                    }
                    ann_helper::optional_destroy(word);
                }
            }
        }
    }
    
    void destroy()
    {
        ann_helper::optional_destroy(static_node_infos_);
        ann_helper::optional_destroy(user_dict_single_chinese_word_);
        dat_trie_.destroy();
    }

    const DatElement* Find(const char *word) const{ return dat_trie_.Find(word); }

    void Find(RuneStrArray::const_iterator begin, RuneStrArray::const_iterator end, Vector<DatDag> &res) const
    {
        dat_trie_.Find(begin, end, res);
    }

    bool IsUserDictSingleChineseWord(const Rune &word) const
    {
        return user_dict_single_chinese_word_.cfind(word) != user_dict_single_chinese_word_.cend();
    }

    double GetMinWeight() const { return dat_trie_.GetMinWeight(); }

    void LoadDict(const char *filePath)
    {
        std::ifstream ifs(filePath);
        if (!ifs.is_open()) {
            ereport(ERROR,
                (errcode(ERRCODE_FILE_READ_FAILED),
                 errmsg("open \"%s\" failed", filePath)));
        }
        constexpr size_t BUFFER_SIZE = 1024;
        char line[BUFFER_SIZE];
        DatElement node_info;
        constexpr size_t dict_args_num = 3;
        char *tokens[dict_args_num] = {NULL};
    
        while (ifs.getline(line, BUFFER_SIZE)) {
            size_t args_num = split(tokens, line, " ", dict_args_num);
            if (args_num != dict_args_num) {
                ereport(ERROR,
                    (errcode(ERRCODE_FILE_READ_FAILED),
                     errmsg("illegal line: \"%s\"", line)));
            }
            node_info.word = pool_->get_token(tokens[0]);
            node_info.weight = atof(tokens[1]);
            static_node_infos_.push_back(node_info);
        }
    }

    static bool WeightCompare(const DatElement &lhs, const DatElement &rhs)
    {
        return lhs.weight < rhs.weight;
    }

    void SetStaticWordWeights(double &min_weight)
    {
        for (DatElement &node : static_node_infos_) {
            min_weight = std::min(min_weight, node.weight);
        }
    }

    double CalcFreqSum(const Vector<DatElement> &node_infos) const
    {
        double sum = 0.0;
        for (const auto &node_info : node_infos) {
            sum += node_info.weight;
        }
        return sum;
    }

    void CalculateWeight(Vector<DatElement> &node_infos, double sum) const
    {
        if (node_infos.size() == 1) {
            node_infos[0].weight = -0.693147;
            return;
        }
        for (auto &node_info : node_infos) {
            Assert(node_info.weight > 0.0);
            node_info.weight = log(double(node_info.weight) / sum);
        }
    }

private:
    bool case_sensitive_;
    bm25_tokenizer::TokenPool *pool_;
    Vector<DatElement> static_node_infos_;
    DatTrie dat_trie_{static_node_infos_};
    double freq_sum_;
    UnorderedSet<Rune> user_dict_single_chinese_word_;
};

}  /* namespace cppjieba */
#endif  /* CPPJIEBA_DICT_TRIE_HPP */
