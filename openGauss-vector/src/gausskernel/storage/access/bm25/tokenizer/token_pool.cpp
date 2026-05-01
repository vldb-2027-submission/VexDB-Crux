/**
 * Copyright ...
 */

#include "utils/palloc.h"
#include "access/bm25/tokenizer/token_pool.h"

using namespace bm25_tokenizer;

TokenPool::TokenPool(MemoryContext ctx) : buffers(ctx) { buffers.push_back(alloc_buffer()); }

void TokenPool::destroy()
{
    for (auto buffer : buffers) {
        auto alloc = buffer_vec_type::allocator_type::rebind<char>::other(buffers.get_allocator());
        alloc.deallocate(buffer, buffer_length);
    }
    buffers.destroy();
}

void TokenPool::reset()
{
    const size_t s = buffers.size();
    for (size_t i = 1ul; i < s; ++i) {
        auto alloc = buffer_vec_type::allocator_type::rebind<char>::other(buffers.get_allocator());
        alloc.deallocate(buffers[i], buffer_length);
    }
    /* we don't need to deal with case s == 0 */
    buffers.resize(1ul);
    cur_offset = 0;
}

const char *TokenPool::get_token(const char *tok, size_t len)
{
    if (len > buffer_length - 1ul) {
        ereport(ERROR, (errmsg("Word is too long for token pool, got length %lu with the max of %lu",
                               len + 1ul, buffer_length)));
    }
    if (cur_offset + len >= buffer_length - 1ul) {
        cur_offset = 0;
        buffers.push_back(alloc_buffer());
    }
    char *res = buffers.back() + cur_offset;
    errno_t rc = memcpy_s(res, buffer_length - cur_offset, tok, len);
    securec_check(rc, "\0", "\0");
    res[len] = '\0';
    cur_offset += len + 1ul;
    return res;
}

FORCE_INLINE char *TokenPool::alloc_buffer()
{
    return buffer_vec_type::allocator_type::rebind<char>::other(
        buffers.get_allocator()).allocate(buffer_length);
}
