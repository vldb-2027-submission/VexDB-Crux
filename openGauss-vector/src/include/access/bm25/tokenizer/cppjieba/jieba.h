/**
 * Copyright ...
 */

#ifndef CPPJIEAB_JIEBA_H
#define CPPJIEAB_JIEBA_H

#include <string.h>

#include "access/bm25/tokenizer/cppjieba/query_segment.h"
#include "tsearch/ts_cache.h"
#include "tsearch/ts_locale.h"
#include "access/bm25/tokenizer/cppjieba/dsnowball.h"
#include "mb/pg_wchar.h"

namespace cppjieba {
class Jieba {
public:
    Jieba(const char *delimiter, const char *dict_path, const char *model_path, const char *stopwords_path,
          bool case_sensitive, ArrayCStr &userdict_arr, ArrayCStr &stopwords_arr)
        {
            use_delimiter_ = delimiter != NULL;
            if (!use_delimiter_) {
                new (&store_) store(dict_path, model_path, case_sensitive, userdict_arr, &pool_);
            } else {
                error_t rc = strncpy_s(delimiter_, sizeof(store), delimiter, strlen(delimiter));
                securec_check_c(rc, "\0", "\0");
            }
            LoadStopWordDict(stopwords_path);
            for (int32 i = 0; i < stopwords_arr.count; ++i) {
                stopwords_.insert(pool_.get_token(stopwords_arr.values[i]));
            }
        }

    void destroy()
    {
        if (!use_delimiter_) {
            store_.destroy();
        }
        ann_helper::optional_destroy(stopwords_);
        pool_.destroy();
    }

    bool is_stopword(const char *word) { return stopwords_.contains(word); }

    template <typename Container>
    Vector<char *> post_processing(const Container &container)
    {
        Vector<char *> keywords;
        for (const auto &elem : container) {
            using elem_type = typename Container::value_type;
            using is_char_ptr = typename std::is_same<elem_type, char *>::type;
            char *orig_word = get_data(elem, is_char_ptr{});
            char *lower_word = to_lower_copy(orig_word);
            if (*lower_word != '\0' && is_stopword(lower_word)) {
                pfree(lower_word);
                continue;
            }
            pfree(lower_word);

            char *word = (char *)pg_do_encoding_conversion(
                (unsigned char *)orig_word, strlen(orig_word), PG_UTF8, GetDatabaseEncoding());
            if (strlen(word) > MAX_WORD_LENGTH) {
                ArrayInt2 sep_index = get_sep_index(word);
                ereport(WARNING,
                    (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
                        errmsg("the length of the tokenize result word:\"%s\" is greater than %lu, "
                               "will be divided", word, MAX_WORD_LENGTH)));
                const char *p = word;
                for (int i = 0; i < sep_index.count - 1; ++i) {
                    int cp_len = sep_index.values[i + 1] - sep_index.values[i];
                    char *slice_word = (char *)palloc(sizeof(char) * (cp_len + 1));
                    errno_t rc = memcpy_s(slice_word, cp_len + 1, p, cp_len);
                    securec_check_c(rc, "\0", "\0");
                    slice_word[cp_len] = '\0';
                    p += cp_len;
                    process_lexeme_result(keywords, static_cast<TSLexeme *>(call_dsnowball_lexize(slice_word)));
                    pfree(slice_word);
                }
            } else {
                process_lexeme_result(keywords, static_cast<TSLexeme *>(call_dsnowball_lexize(word)));
            }
            if (word != orig_word) {
                pfree(word);
            }
        }
        return keywords;
    }

    Vector<Word> cut_delimiter(const char *sentence)
    {
        Vector<Word> keywords;
        if (sentence == NULL) {
            return keywords;
        }
        const char *start = sentence;
        const char *current = sentence;
        size_t del_len = strlen(delimiter_);
        while (*current != '\0') {
            bool is_delimiter = true;
            for (size_t i = 0; i < del_len; ++i) {
                if (current[i] == '\0' || current[i] != delimiter_[i]) {
                    is_delimiter = false;
                    break;
                }
            }
            if (is_delimiter) {
                const size_t word_len = current - start;
                if (word_len > 0) {
                    char *word = (char *)palloc(word_len + 1);
                    errno_t rc = strncpy_s(word, word_len + 1, start, word_len);
                    securec_check_c(rc, "\0", "\0");
                    word[word_len] = '\0';
                    uint32 offset = start - sentence;
                    keywords.emplace_back(word, offset);
                }
                current += del_len;
                start = current;
            } else {
                ++current;
            }
        }
        const size_t last_word_len = current - start;
        if (last_word_len > 0) {
            char *word = (char *)palloc(last_word_len + 1);
            errno_t rc = strncpy_s(word, last_word_len + 1, start, last_word_len);
            securec_check_c(rc, "\0", "\0");
            word[last_word_len] = '\0';
            uint32 offset = start - sentence;
            keywords.emplace_back(word, offset);
        }
        return keywords;
    }

    Vector<char *> cut_mix(char *sentence)
    {
        Vector<char *> res;
        Vector<Word> words;
        if (use_delimiter_) {
            words = cut_delimiter(sentence);
        } else {
            char *str = (char *)pg_do_encoding_conversion((unsigned char *)sentence, strlen(sentence),
                GetDatabaseEncoding(), PG_UTF8);
            if (!store_.case_sensitive_) {
                str = pg_strtolower(str);
            }
            store_.mix_seg_.CutToWord(str, words);
            if (str != sentence) {
                pfree(str);
            }
        }
        res = post_processing(words);
        destroy_str_container(words);
        return res;
    }

    Vector<char *> cut_query(char *sentence)
    {
        Vector<char *> res;
        Vector<Word> words;
        if (use_delimiter_) {
            words = cut_delimiter(sentence);
        } else {
            char *str = (char *)pg_do_encoding_conversion((unsigned char *)sentence, strlen(sentence),
                GetDatabaseEncoding(), PG_UTF8);
            if (!store_.case_sensitive_) {
                str = pg_strtolower(str);
            }
            store_.query_seg_.CutToWord(str, words);
            if (str != sentence) {
                pfree(str);
            }
        }
        res = post_processing(words);
        destroy_str_container(words);
        return res;
    }

    char *highlight(char *text, char *query, char *lchar, char *rchar)
    {
        char *text_str = (char *)pg_do_encoding_conversion(
            (unsigned char *)text, strlen(text), GetDatabaseEncoding(), PG_UTF8);
        char *query_str = (char *)pg_do_encoding_conversion(
            (unsigned char *)query, strlen(query), GetDatabaseEncoding(), PG_UTF8);
        Vector<Word> words_text;
        Vector<Word> words_query;
        if (use_delimiter_) {
            words_text = cut_delimiter(text_str);
            words_query = cut_delimiter(query_str);
        } else {
            if (!store_.case_sensitive_) {
                text_str = pg_strtolower(text_str);
                query_str = pg_strtolower(query_str);
            }
            store_.query_seg_.CutToWord(query_str, words_query);
            store_.mix_seg_.CutToWord(text_str, words_text);
        }
        return highlight_process(text, text_str, lchar, rchar, words_text, words_query);
    }

    bool Find(const char *word) { return store_.dict_trie_.Find(word) != NULL; }

    void LoadStopWordDict(const char *filePath)
    {
        if (!filePath) {
            return;
        }
        std::ifstream ifs(filePath);
        constexpr size_t MAX_LINE_LEN = 1024ul;
        char buffer[MAX_LINE_LEN];

        while (ifs.getline(buffer, MAX_LINE_LEN)) {
            stopwords_.insert(pool_.get_token(buffer));
        }
    }

private:
    bool use_delimiter_;
    UnorderedSet<CharString> stopwords_;
    bm25_tokenizer::TokenPool pool_;

    struct store {
        DictTrie dict_trie_;
        HMMModel model_;
        MPSegment mp_seg_;
        HMMSegment hmm_seg_;
        MixSegment mix_seg_;
        FullSegment full_seg_;
        QuerySegment query_seg_;
        bool case_sensitive_;

        store(const char *dict_path, const char *model_path, bool case_sensitive,
              ArrayCStr &userdict_arr, bm25_tokenizer::TokenPool *pool_)
        : dict_trie_(dict_path, userdict_arr, case_sensitive, pool_),
          model_(model_path),
          mp_seg_(&dict_trie_),
          hmm_seg_(&model_),
          mix_seg_(&dict_trie_, &model_),
          full_seg_(&dict_trie_),
          query_seg_(&dict_trie_, &model_),
          case_sensitive_(case_sensitive) {}

        void destroy()
        {
            dict_trie_.destroy();
            model_.destroy();
            mp_seg_.destroy();
            hmm_seg_.destroy();
            mix_seg_.destroy();
            full_seg_.destroy();
            query_seg_.destroy();
        }
    };
    union {
        char delimiter_[sizeof(store)];
        store store_;
    };

    bool lexize_word_check(char *lexize_word)
    {
        return lexize_word && !(t_isspace(lexize_word) && lexize_word[pg_mblen(lexize_word)] == '\0');
    }

    char *to_lower_copy(const char *src)
    {
        if (!src) {
            return NULL;
        }
        char *dest = pstrdup(src);
        dest = pg_strtolower(dest);
        return dest;
    }

    void process_lexeme_result(Vector<char *> &keywords, TSLexeme *res)
    {
        char *lexize_word = res->lexeme;
        pfree(res);
        if (lexize_word_check(lexize_word)) {
            keywords.emplace_back(lexize_word);
        } else if (lexize_word) {
            pfree(lexize_word);
        }
    }

    template <typename DataType>
    char *get_data(DataType &d, std::false_type) { return const_cast<char *>(d.word); }
    template <typename DataType>
    char *get_data(DataType &d, std::true_type) { return d; }

    char *highlight_process(char *text, char *text_str, char *lchar, char *rchar,
        Vector<Word> &words_text, Vector<Word> &words_query)
    {
        int db_encoding = GetDatabaseEncoding();
        struct Match {
            uint32 off;
            uint32 len;
            Match(uint32 o, uint32 l) : off(o), len(l) {}
        };
        Vector<Match> match;
        for (Word &elem_q : words_query) {
            char *word_q = const_cast<char *>(elem_q.word);
            char *lower_q = to_lower_copy(word_q);
            if (*lower_q != '\0' && is_stopword(lower_q)) {
                pfree(lower_q);
                continue;
            }
            pfree(lower_q);
            char *word_qq = (char *)pg_do_encoding_conversion(
                (unsigned char *)word_q, strlen(word_q), PG_UTF8, db_encoding);
            TSLexeme *res_q = (TSLexeme *)call_dsnowball_lexize(word_qq);
            char *lexize_q = res_q->lexeme;
            pfree(res_q);
            if (!lexize_word_check(lexize_q)) {
                pfree(lexize_q);
                continue;
            }
            size_t len_q = strlen(lexize_q);

            for (Word &elem_t : words_text) {
                char *word_t = const_cast<char *>(elem_t.word);
                char *lower_t = to_lower_copy(word_t);
                if (*lower_t != '\0' && is_stopword(lower_t)) {
                    pfree(lower_t);
                    continue;
                }
                pfree(lower_t);
                char *word_tt = (char *)pg_do_encoding_conversion(
                    (unsigned char *)word_t, strlen(word_t), PG_UTF8, db_encoding);
                TSLexeme *res_t = (TSLexeme *)call_dsnowball_lexize(word_tt);
                char *lexize_t = res_t->lexeme;
                pfree(res_t);
                if (!lexize_word_check(lexize_t)) {
                    pfree(lexize_t);
                    continue;
                }

                /* do match check */
                if (len_q == strlen(lexize_t) && pg_strncasecmp(lexize_q, lexize_t, len_q) == 0) {
                    match.emplace_back(elem_t.offset, strlen(word_t));
                }

                if (word_tt != word_t) {
                    pfree(word_tt);
                }
            }
            if (word_qq != word_q) {
                pfree(word_qq);
            }
        }

        uint32 src_pos = 0;
        uint32 dst_pos = 0;
        size_t match_idx = 0;
        uint32 s_len = strlen(text);
        uint32 lchar_len = strlen(lchar);
        uint32 rchar_len = strlen(rchar);
        errno_t rc;
        size_t max_len = 2 * (s_len + (lchar_len + rchar_len) * match.size());
        char *result = (char *)palloc(max_len);
        std::sort(match.begin(), match.end(), [](const Match &a, const Match &b) {
            return a.off < b.off;
        });
        while (src_pos < s_len && match_idx < match.size()) {
            Match m = match[match_idx++];
            if (m.off < src_pos) {
                continue;
            }
            if (src_pos < m.off) {
                int copy_len = m.off - src_pos;
                rc = memcpy_s(result + dst_pos, max_len, text_str + src_pos, copy_len);
                securec_check_c(rc, "\0", "\0");
                dst_pos += copy_len;
                src_pos = m.off;
            }
            /* add lchar */
            rc = memcpy_s(result + dst_pos, max_len, lchar, lchar_len);
            securec_check_c(rc, "\0", "\0");
            dst_pos += lchar_len;
            /* copy keyword */
            rc = memcpy_s(result + dst_pos, max_len, text_str + src_pos, m.len);
            securec_check_c(rc, "\0", "\0");
            dst_pos += m.len;
            src_pos += m.len;
            /* add rchar */
            rc = memcpy_s(result + dst_pos, max_len, rchar, rchar_len);
            securec_check_c(rc, "\0", "\0");
            dst_pos += rchar_len;
        }
        if (src_pos < s_len) {
            int remain_len = s_len - src_pos;
            rc = memcpy_s(result + dst_pos, max_len, text_str + src_pos, remain_len);
            securec_check_c(rc, "\0", "\0");
            dst_pos += remain_len;
        }
        result[dst_pos] = '\0';
        char *res = (char *)pg_do_encoding_conversion(
            (unsigned char *)result, strlen(result), PG_UTF8, db_encoding);
        return res;
    }
};

}  /* namespace cppjieba */
#endif  /* CPPJIEAB_JIEBA_H */
