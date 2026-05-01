/************************************
 * file enc : ascii
 * author   : wuyanyi09@gmail.com
 ************************************/

#ifndef LIMONP_STR_FUNCTS_H
#define LIMONP_STR_FUNCTS_H

#include <functional>
#include <vtl/vector>
#include "utils/palloc.h"
#include "tsearch/ts_locale.h"
#include "access/bm25/tokenizer/cppjieba/unicode.h"

constexpr char KEYWORD_SEP = ',';
constexpr char keyword_sep[] = {KEYWORD_SEP, '\0'};
constexpr char SLICE_SEP = '|';
constexpr char slice_sep[] = {SLICE_SEP, '\0'};
constexpr size_t MAX_WORD_LENGTH = 2000ul;

inline bool is_positive_integer(const char *str) {
    if (str == NULL || *str == '\0') {
        return false;
    }
    while (*str == ' ') {
        ++str;
    }
    if (*str == '-') {
        return false;
    }
    if (*str == '+') {
        ++str;
    }
    const char* p = str;
    while (*p != '\0') {
        if (!isdigit(*p)) {
            return false;
        }
        ++p;
    }
    if (str[0] == '0' && strlen(str) > 1) {
        return false;
    }
    long num = 0;
    for (p = str; *p != '\0'; ++p) {
        int digit = *p - '0';
        if (num > (LONG_MAX - digit) / 10) {
            return false;
        }
        num = num * 10 + digit;
    }
    return num > 0 && num <= INT_MAX;
}

inline size_t split(char **tokens, char *s, const char *delim, size_t max_split)
{
    char *token = NULL;
    char *save_ptr = NULL;
    size_t row = 0;
    token = strtok_s(s, delim, &save_ptr);
    while (token != NULL && row < max_split) {
        tokens[row++] = token;
        token = strtok_s(NULL, delim, &save_ptr);
    }
    return row;
}

inline char *insert_cstr(char *src, ArrayInt2 &sep_index, char sep)
{
    int total_len = strlen(src) + (sep_index.count - 2) + 1;
    sep_index.values[sep_index.count - 1] = strlen(src);
    char *res = (char *)palloc(sizeof(char) * total_len);
    char *p1 = res;
    const char *p2 = src;
    for (int i = 0; i < sep_index.count - 1; ++i) {
        int cp_len = sep_index.values[i + 1] - sep_index.values[i];
        errno_t rc = memcpy_s(p1, total_len, p2, cp_len);
        securec_check_c(rc, "\0", "\0");
        p1[cp_len] = sep;
        p1 += cp_len + 1;
        p2 += cp_len;
    }
    res[total_len - 1] = '\0';
    return res;
}

inline ArrayInt2 get_sep_index(const char *src)
{
    int src_len = strlen(src);
    int sep_num = (src_len - 1) / MAX_WORD_LENGTH;

    ArrayInt2 sep_index;
    sep_index.count = sep_num + 2; /* head and tail sentinel */
    sep_index.values = (int2 *)palloc(sizeof(int2) * sep_index.count);
    sep_index.values[0] = 0;
    sep_index.values[sep_index.count - 1] = strlen(src);
    
    const char *p = src;
    int remaining = src_len;
    
    for (int2 i = 1; i < sep_index.count - 1; ++i) {
        int chunk_len = pg_mbcliplen(p, remaining, MAX_WORD_LENGTH);
        sep_index.values[i] = sep_index.values[i - 1] + chunk_len;
        p += chunk_len;
        remaining -= chunk_len;
    }
    return sep_index;
}

inline void converse_encoding_arraycstr(ArrayCStr &a)
{
    int db_encoding = GetDatabaseEncoding();
    for (int32 i = 0; i < a.count; ++i) {
        char *orig_word = a.values[i];
        char *word = (char *)pg_do_encoding_conversion(
            (unsigned char *)orig_word, strlen(orig_word), db_encoding, PG_UTF8);
        if (word != orig_word) {
            pfree(orig_word);
            a.values[i] = word;
        }
    }
}

inline void check_input(ArrayCStr &a)
{
    for (int32 i = 0; i < a.count; ++i) {
        char *line = pstrdup(a.values[i]);
        constexpr size_t userdict_args_num = 2;
        size_t delim_count = 0;
        size_t len = strlen(line);
        for (size_t j = 0; j < len; ++j) {
            if (line[j] == KEYWORD_SEP) {
                ++delim_count;
            }
        }
        if (delim_count > userdict_args_num - 1) {
            ereport(ERROR,
                (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
                    errmsg("too many args in line: \"%s\".", a.values[i])));
        }
        char *tokens[userdict_args_num] = {NULL};
        size_t args_num = split(tokens, line, keyword_sep, userdict_args_num);
        if (tokens[0] == NULL || *tokens[0] == '\0') {
            ereport(ERROR,
                (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
                    errmsg(
                        "invalid line: \"%s\", keyword cannot be empty", a.values[i])));
        }
        ArrayInt2 sep_index;
        sep_index.count = 0;
        if (strlen(tokens[0]) > MAX_WORD_LENGTH) {
            ereport(WARNING,
                (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
                    errmsg("the length of the keyword \"%s\" is greater than %lu, "
                           "will be divided", tokens[0], MAX_WORD_LENGTH)));
            sep_index = get_sep_index(tokens[0]);
        }
        if (args_num == 2) {
            if (!is_positive_integer(tokens[1])){
                ereport(ERROR,
                    (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
                        errmsg(
                            "invalid second value \"%s\" in line: \"%s\","
                            "should be positive integer", tokens[1], a.values[i])));
            }
        }
        if (sep_index.count > 0) {
            char *res = insert_cstr(a.values[i], sep_index, SLICE_SEP);
            pfree(a.values[i]);
            a.values[i] = res;
        }
        pfree(line);
    }
}

inline void free_str_container(Vector<char *> &container)
{
    for (char *s : container) {
        pfree(s);
    }
    container.clear();
}

inline void destroy_str_container(Vector<cppjieba::Word> &container)
{
    for (cppjieba::Word &w : container) {
        pfree((void *)w.word);
    }
    ann_helper::optional_destroy(container);
}

class CharString {
    const char *_data;

public:
    CharString(const char* str) : _data(str) {} /* implicit */

    char *str() const noexcept { return const_cast<char *>(_data); }
    const char *c_str() const noexcept { return _data; }

    bool operator==(const CharString &rhs) const
    {
        if (_data == NULL || rhs._data == NULL) {
            return _data == rhs._data;
        }
        return strcmp(_data, rhs._data) == 0;
    }
    bool operator!=(const CharString &rhs) const { return !(*this == rhs); }

    bool operator<(const CharString &rhs) const
    {
        if (_data == NULL) {
            return rhs._data != NULL;
        }
        if (rhs._data == NULL) {
            return false;
        }
        return strcmp(_data, rhs._data) < 0;
    }
    bool operator>(const CharString &rhs) const { return rhs < *this; }
    bool operator<=(const CharString &rhs) const { return !(rhs < *this); }
    bool operator>=(const CharString &rhs) const { return !(*this < rhs); }
};

template <>
struct std::hash<CharString> {
    size_t operator()(const CharString &cs) const noexcept
    {
        if (!cs.c_str()) {
            return 0;
        }
        size_t hash = 5381ul;
        const char *ptr = cs.c_str();
        while (*ptr) {
            hash = ((hash << 5) + hash) + *ptr++;
        }
        return hash;
    }
};

inline char *ascii_trim(char *str)
{
    Assert(str);

    char *start = str;
    while (isspace((unsigned char)*start)) {
        ++start;
    }

    char *end = start + strlen(start) - 1;
    while (end > start && isspace((unsigned char)*end)) {
        --end;
    }
    end[1] = '\0';
    return start;
}

inline void rtrim(char *buf)
{
    Assert(buf);

    int nspace = 0;
    while (*buf) {
        const int clen = pg_mblen(buf);
        if (t_isspace(buf)) {
            nspace += clen;
        } else {
            nspace = 0;
        }
        buf += clen;
    }
    buf[-nspace] = '\0';
}

inline char *trim(char *buf)
{
    Assert(buf);

    char *start = buf;
    while (t_isspace(start)) {
        start += pg_mblen(start);
    }

    rtrim(start);
    return start;
}

#endif  /* LIMONP_STR_FUNCTS_H */
