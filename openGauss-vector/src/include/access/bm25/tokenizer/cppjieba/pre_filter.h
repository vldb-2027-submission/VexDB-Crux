/**
 * Copyright ...
 */

#ifndef CPPJIEBA_PRE_FILTER_H
#define CPPJIEBA_PRE_FILTER_H

namespace cppjieba {
class PreFilter {
public:
    struct Range {
        RuneStrArray::const_iterator begin;
        RuneStrArray::const_iterator end;
        Range(RuneStrArray::const_iterator l, RuneStrArray::const_iterator r) : begin(l), end(r) {}
        size_t Length() const { return end - begin + 1; }
        bool IsAllAscii() const
        {
            for (auto iter = begin; iter <= end; ++iter) {
                if (iter->rune >= 0x80) {
                    return false;
                }
            }
            return true;
        }
    };

    PreFilter(const UnorderedSet<Rune> &symbols, const char *sentence) : symbols_(symbols)
    {
        if (!DecodeUTF8RunesInString(sentence, sentence_)) {
            ereport(ERROR,
                (errcode(ERRCODE_SYNTAX_ERROR),
                 errmsg("Decode \"%s\" failed", sentence)));
        }
        cursor_ = sentence_.begin();
    }
    void destroy()
    {
        cursor_ = NULL;
        sentence_.destroy();
    }

    bool HasNext() const{ return cursor_ != sentence_.end(); }
    Range Next()
    {
        Range range(cursor_, cursor_);
        while (cursor_ != sentence_.end()) {
            if (symbols_.cend() != symbols_.cfind(cursor_->rune)) {
                if (range.begin == cursor_) {
                    cursor_++;
                }
                range.end = cursor_;
                return range;
            }
            cursor_++;
        }
        range.end = sentence_.end();
        return range;
    }

private:
    RuneStrArray::const_iterator cursor_;
    RuneStrArray sentence_;
    const UnorderedSet<Rune> &symbols_;
};
}  /* namespace cppjieba */
#endif  /* CPPJIEBA_PRE_FILTER_H */
