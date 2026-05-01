/**
 * Copyright ...
 */

#ifndef CPPJIEBA_UNICODE_H
#define CPPJIEBA_UNICODE_H

#include <iterator>
#include <vtl/vector>
#include "utils/palloc.h"
#include "access/bm25/tokenizer/token.h"

namespace cppjieba {

typedef uint32 Rune;

struct Word {
    const char *word;
    uint32 offset;
    uint32 unicode_offset;
    uint32 unicode_length;
    Word(const char *w, uint32 o) : word(w), offset(o) {}
    Word(const char *w, uint32 o, uint32 unicode_offset, uint32 unicode_length)
        : word(w), offset(o), unicode_offset(unicode_offset), unicode_length(unicode_length) {}
};

struct RuneStr {
    Rune rune;
    uint32 offset;
    uint32 len;
    uint32 unicode_offset;
    uint32 unicode_length;
    RuneStr() : rune(0), offset(0), len(0), unicode_offset(0), unicode_length(0) {}
    RuneStr(Rune r, uint32 o, uint32 l) : rune(r), offset(o), len(l), unicode_offset(0), unicode_length(0) {}
    RuneStr(Rune r, uint32 o, uint32 l, uint32 unicode_offset, uint32 unicode_length)
        : rune(r), offset(o), len(l), unicode_offset(unicode_offset), unicode_length(unicode_length) {}
};

using Unicode = Vector<Rune>;
using RuneArray = Unicode;
using RuneStrArray = Vector<RuneStr>;

struct WordRange {
    RuneStrArray::const_iterator left;
    RuneStrArray::const_iterator right;
    WordRange(RuneStrArray::const_iterator l, RuneStrArray::const_iterator r) : left(l), right(r) {}
    size_t Length() const { return std::distance(left, right) + 1ul; }
    bool IsAllAscii() const
    {
        for (RuneStrArray::const_iterator iter = left; iter <= right; ++iter) {
            if (iter->rune >= 0x80) {
                return false;
            }
        }
        return true;
    }
};

struct RuneStrLite {
    uint32 rune;
    uint32 len;
    RuneStrLite() : rune(0), len(0) {}
    RuneStrLite(uint32 r, uint32 l) : rune(r), len(l) {}
};

inline RuneStrLite DecodeUTF8ToRune(const char *str, size_t len)
{
    RuneStrLite rp(0, 0);
    if (str == NULL || len == 0) {
        return rp;
    }
    if (!(str[0] & 0x80)) {  // 0xxxxxxx
        // 7bit, total 7bit
        rp.rune = (uint8_t)(str[0]) & 0x7f;
        rp.len = 1;
    } else if ((uint8_t)str[0] <= 0xdf && 1 < len) {
        // 110xxxxxx
        // 5bit, total 5bit
        rp.rune = (uint8_t)(str[0]) & 0x1f;

        // 6bit, total 11bit
        rp.rune <<= 6;
        rp.rune |= (uint8_t)(str[1]) & 0x3f;
        rp.len = 2;
    } else if ((uint8_t)str[0] <= 0xef && 2 < len) {  // 1110xxxxxx
        // 4bit, total 4bit
        rp.rune = (uint8_t)(str[0]) & 0x0f;

        // 6bit, total 10bit
        rp.rune <<= 6;
        rp.rune |= (uint8_t)(str[1]) & 0x3f;

        // 6bit, total 16bit
        rp.rune <<= 6;
        rp.rune |= (uint8_t)(str[2]) & 0x3f;

        rp.len = 3;
    } else if ((uint8_t)str[0] <= 0xf7 && 3 < len) {  // 11110xxxx
        // 3bit, total 3bit
        rp.rune = (uint8_t)(str[0]) & 0x07;

        // 6bit, total 9bit
        rp.rune <<= 6;
        rp.rune |= (uint8_t)(str[1]) & 0x3f;

        // 6bit, total 15bit
        rp.rune <<= 6;
        rp.rune |= (uint8_t)(str[2]) & 0x3f;

        // 6bit, total 21bit
        rp.rune <<= 6;
        rp.rune |= (uint8_t)(str[3]) & 0x3f;

        rp.len = 4;
    } else {
        rp.rune = 0;
        rp.len = 0;
    }
    return rp;
}

inline bool DecodeUTF8RunesInString(const char *s, size_t len, RuneStrArray &runes)
{
    runes.clear();
    runes.reserve(len / 2);
    for (uint32 i = 0, j = 0; i < len;) {
        RuneStrLite rp = DecodeUTF8ToRune(s + i, len - i);
        if (rp.len == 0) {
            runes.clear();
            return false;
        }
        RuneStr x(rp.rune, i, rp.len, j, 1);
        runes.push_back(x);
        i += rp.len;
        ++j;
    }
    return true;
}

inline bool DecodeUTF8RunesInString(const char *s, RuneStrArray &runes)
{
    return DecodeUTF8RunesInString(s, strlen(s), runes);
}

inline bool DecodeUTF8RunesInString(const char *s, size_t len, Unicode &unicode)
{
    unicode.clear();
    RuneStrArray runes;
    if (!DecodeUTF8RunesInString(s, len, runes)) {
        return false;
    }
    unicode.reserve(runes.size());
    for (size_t i = 0; i < runes.size(); i++) {
        unicode.push_back(runes[i].rune);
    }
    runes.destroy();
    return true;
}

inline bool DecodeUTF8RunesInString(const char *s, Unicode &unicode)
{
    return DecodeUTF8RunesInString(s, strlen(s), unicode);
}

inline Unicode DecodeUTF8RunesInString(const char *s)
{
    Unicode result;
    DecodeUTF8RunesInString(s, result);
    return result;
}

inline Word GetWordFromRunes(const char *s, RuneStrArray::const_iterator left, RuneStrArray::const_iterator right)
{
    Assert(right->offset >= left->offset);
    uint32 len = right->offset - left->offset + right->len;
    uint32 unicode_length = right->unicode_offset - left->unicode_offset + right->unicode_length;

    char *substr = (char *)palloc(len + 1);
    strncpy(substr, s + left->offset, len);
    substr[len] = '\0';

    return Word(substr, left->offset, left->unicode_offset, unicode_length);
}

inline void GetWordsFromWordRanges(const char *s, const Vector<WordRange> &wrs, Vector<Word> &words)
{
    for (size_t i = 0; i < wrs.size(); i++) {
        words.push_back(GetWordFromRunes(s, wrs[i].left, wrs[i].right));
    }
}

inline int UnicodeToUtf8Bytes(uint32 ui)
{
    if (ui <= 0x7f) {
        return 1;
    }
    if (ui <= 0x7ff) {
        return 2;
    }
    if (ui <= 0xffff) {
        return 3;
    }
    return 4;
}

class RunePtrWrapper {
public:
    const RuneStr *m_ptr = NULL;
public:
    explicit RunePtrWrapper(const RuneStr * p) : m_ptr(p) {}
    uint32_t operator *() { return m_ptr->rune; }
    RunePtrWrapper operator++(int) {
        m_ptr++;
        return RunePtrWrapper(m_ptr);
    }
    bool operator !=(const RunePtrWrapper & b) const { return this->m_ptr != b.m_ptr; }
};

template <class Uint32ContainerConIter>
char* Unicode32ToUtf8(Uint32ContainerConIter begin, Uint32ContainerConIter end, size_t& out_len) {
    size_t buffer_size = 0;
    for (auto it = begin; it != end; it++) {
        uint32_t ui = *it;
        if (ui <= 0x7F) {
            buffer_size += 1;
        } else if (ui <= 0x7FF) {
            buffer_size += 2;
        } else if (ui <= 0xFFFF) {
            buffer_size += 3;
        } else if (ui <= 0x10FFFF) {  // Unicode max value
            buffer_size += 4;
        } else {
            ereport(ERROR,
                (errcode(ERRCODE_SYNTAX_ERROR),
                 errmsg("Invalid Unicode code point")));
        }
    }

    char* buffer = (char*)palloc(sizeof(char) * (buffer_size + 1));
    char* ptr = buffer;
    while (begin != end) {
        uint32_t ui = *begin;
        if (ui <= 0x7F) {
            *ptr++ = static_cast<char>(ui);
        } else if (ui <= 0x7FF) {
            *ptr++ = static_cast<char>(((ui >> 6) & 0x1F) | 0xC0);
            *ptr++ = static_cast<char>((ui & 0x3F) | 0x80);
        } else if (ui <= 0xFFFF) {
            *ptr++ = static_cast<char>(((ui >> 12) & 0x0F) | 0xE0);
            *ptr++ = static_cast<char>(((ui >> 6) & 0x3F) | 0x80);
            *ptr++ = static_cast<char>((ui & 0x3F) | 0x80);
        } else {
            *ptr++ = static_cast<char>(((ui >> 18) & 0x03) | 0xF0);
            *ptr++ = static_cast<char>(((ui >> 12) & 0x3F) | 0x80);
            *ptr++ = static_cast<char>(((ui >> 6) & 0x3F) | 0x80);
            *ptr++ = static_cast<char>((ui & 0x3F) | 0x80);
        }
        begin++;
    }

    *ptr = '\0';
    out_len = buffer_size;
    return buffer;
}

inline char *EncodeRunesToString(RuneStrArray::const_iterator begin, RuneStrArray::const_iterator end) {
    RunePtrWrapper it_begin(begin), it_end(end);
    size_t len = 0;
    char* str = Unicode32ToUtf8(it_begin, it_end, len);
    return str;
}

class Unicode32Counter {
public :
    size_t length = 0;
    void clear() {
        length = 0;
    }
    void push_back(uint32) {
        ++length;
    }
};

template <class Uint32Container>
bool Utf8ToUnicode32(const char * str, size_t size, Uint32Container& vec) {
    uint32 tmp;
    vec.clear();
    for(size_t i = 0; i < size;) {
        if(!(str[i] & 0x80)) { // 0xxxxxxx
            // 7bit, total 7bit
            tmp = (uint8_t)(str[i]) & 0x7f;
            i++;
        } else if ((uint8_t)str[i] <= 0xdf && i + 1 < size) { // 110xxxxxx
            // 5bit, total 5bit
            tmp = (uint8_t)(str[i]) & 0x1f;

            // 6bit, total 11bit
            tmp <<= 6;
            tmp |= (uint8_t)(str[i+1]) & 0x3f;
            i += 2;
        } else if((uint8_t)str[i] <= 0xef && i + 2 < size) { // 1110xxxxxx
            // 4bit, total 4bit
            tmp = (uint8_t)(str[i]) & 0x0f;

            // 6bit, total 10bit
            tmp <<= 6;
            tmp |= (uint8_t)(str[i+1]) & 0x3f;

            // 6bit, total 16bit
            tmp <<= 6;
            tmp |= (uint8_t)(str[i+2]) & 0x3f;

            i += 3;
        } else if((uint8_t)str[i] <= 0xf7 && i + 3 < size) { // 11110xxxx
            // 3bit, total 3bit
            tmp = (uint8_t)(str[i]) & 0x07;

            // 6bit, total 9bit
            tmp <<= 6;
            tmp |= (uint8_t)(str[i+1]) & 0x3f;

            // 6bit, total 15bit
            tmp <<= 6;
            tmp |= (uint8_t)(str[i+2]) & 0x3f;

            // 6bit, total 21bit
            tmp <<= 6;
            tmp |= (uint8_t)(str[i+3]) & 0x3f;

            i += 4;
        } else {
            return false;
        }
        vec.push_back(tmp);
    }
    return true;
}
    
inline size_t Utf8CharNum(const char *str, size_t length) {
    Unicode32Counter c;
    if (Utf8ToUnicode32(str, length, c)) {
        return c.length;
    }
    return 0;
}

inline bool DecodeRunesInString(const char* s, RuneArray& arr) {
    arr.clear();
    return Utf8ToUnicode32(s, strlen(s), arr);
}

inline bool DecodeRunesInString(const char* s, RuneStrArray& runes) {
    RuneArray arr;
    if (!DecodeRunesInString(s, arr)) {
        return false;
    }
    runes.clear();
    uint32 offset = 0;
    for (uint32 i = 0; i < arr.size(); ++i) {
        const uint32 len = UnicodeToUtf8Bytes(arr[i]);
        RuneStr x(arr[i], offset, len, i, 1);
        runes.push_back(x);
        offset += len;
    }
    return true;
}

}  /* namespace cppjieba */
#endif  /* CPPJIEBA_UNICODE_H */
