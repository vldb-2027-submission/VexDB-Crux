/**
 * Copyright ...
 */

#ifndef BM25_TOKENIZER_TOKENPOOL_H
#define BM25_TOKENIZER_TOKENPOOL_H

#include <vtl/vector>

#include "c.h"
#include "access/bm25/tokenizer/token.h"

namespace bm25_tokenizer {
class TokenPool {
    using buffer_vec_type = Vector<char *, CONTEXT_ALLOCATOR<char *>>;
public:
    explicit TokenPool(MemoryContext ctx = CurrentMemoryContext);
    void destroy();
    void reset();
    const char *get_token(const char *tok, size_t len);
    const char *get_token(const char *tok) { return get_token(tok, strlen(tok)); }
private:
    constexpr static size_t buffer_length = max_token_length * 4ul;
    size_t cur_offset{0};
    buffer_vec_type buffers;

    char *alloc_buffer();
};
} /* namespace bm25_tokenizer */

#endif /* BM25_TOKENIZER_TOKENPOOL_H */
