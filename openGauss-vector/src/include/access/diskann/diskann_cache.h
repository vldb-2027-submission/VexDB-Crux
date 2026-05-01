/**
 * Copyright ...
 * Global PQ cache storing centers for vector indexes.
 */

#ifndef DISKANN_CACHE_H
#define DISKANN_CACHE_H

#include <new>  /* replacement new */
#include "c.h"

#define DISKANN_CTX (g_instance.diskann_cxt)
/* thread unsafe */
#define CACHE_ALLOC(size)   \
    MemoryContextMemalignAllocDebug(DISKANN_CTX.ctx, ann_helper::vector_aligned_size, (size), __FILE__, __LINE__)
#define CACHE_FREE(ptr) MemoryContextMemalignFree(DISKANN_CTX.ctx, (ptr))
#define CACHE_FREE_EXT(ptr) \
    do {                    \
        if (ptr) {          \
            CACHE_FREE(ptr); \
            ptr = NULL;     \
        }                   \
    } while (0)
constexpr size_t MAX_ARR_CACHE_SIZE = 64ul;
constexpr size_t DEFAULT_LRU_CACHE_SIZE = 128ul;
constexpr size_t DEFAULT_VECTOR_BUFFERS = 3 * 1024ul * 1024ul;

typedef struct DiskAnnPQCache {
    float *pivots;
    void destroy();
    DiskAnnPQCache() = default;
    DiskAnnPQCache(float *pivots) : pivots(pivots) {}
    DiskAnnPQCache(const DiskAnnPQCache &other)
    {
        pivots = other.pivots;
    }
    DiskAnnPQCache(DiskAnnPQCache &&other)
    {
        pivots = other.pivots;
        other.pivots = NULL;
    }
    inline void operator=(DiskAnnPQCache &&other)
    {
        new (this) DiskAnnPQCache(other);
    }
    inline void operator=(const DiskAnnPQCache &other)
    {
        new (this) DiskAnnPQCache(other);
    }
} DiskAnnPQCache;

typedef struct DiskAnnPQCacheEntry {
    Oid oid;
    DiskAnnPQCache cache;
} DiskAnnPQCacheEntry;

typedef struct DiskAnnPQCacheParameter {
    uint32 num_centers;
    uint32 num_pq_chunks;
    uint32 dim;
    uint32 pivot_blkno = InvalidBlockNumber;
} DiskAnnPQCacheParameter;

/* thread unsafe, use rwlock for access */
const DiskAnnPQCache *diskann_get_pq_cache(Oid index_oid);
bool diskann_set_pq_cache(Oid index_oid, float *pivots);
bool diskann_clear_pq_cache(Oid index_oid);
void diskann_resize_cache(int new_size);
#endif
