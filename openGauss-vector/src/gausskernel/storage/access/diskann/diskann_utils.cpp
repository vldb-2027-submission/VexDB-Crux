#include <vtl/lrucache.hpp>
#include "access/diskann/diskann_internal.h"
#include "access/diskann/diskann_cache.h"
#include "access/nbtree.h"

using lru_cache_type = vtl::LRUCache<Oid, DiskAnnPQCache>;
#define DISKANN_LRU_CACHE_PTR (DISKANN_CTX.lru_cache)
#define DISKANN_LRU_CACHE ((lru_cache_type *)DISKANN_LRU_CACHE_PTR)

void DiskAnnPQCache::destroy()
{
    CACHE_FREE_EXT(pivots);
}

const DiskAnnPQCache *diskann_get_pq_cache(Oid index_oid)
{
    if (!DISKANN_LRU_CACHE) {
        for (int i = 0; i < DISKANN_CTX.cache_size; ++i) {
            if (DISKANN_CTX.diskann_pq_cache[i].oid == index_oid) {
                return &DISKANN_CTX.diskann_pq_cache[i].cache;
            }
        }
        return NULL;
    }
    return DISKANN_LRU_CACHE->get(index_oid);
}

/* return false if cache already exists.
 * pq code will never change, for index rebuilding, call clear cache before resetting */
bool diskann_set_pq_cache(Oid index_oid, float *pivots)
{
    if (!DISKANN_LRU_CACHE) {
        for (int i = 0; i < DISKANN_CTX.cache_size; ++i) {
            if (DISKANN_CTX.diskann_pq_cache[i].oid == index_oid) {
                return false;
            }
        }
        if (size_t(DISKANN_CTX.cache_size) < MAX_ARR_CACHE_SIZE) {
            DISKANN_CTX.diskann_pq_cache[DISKANN_CTX.cache_size].oid = index_oid;
            DISKANN_CTX.diskann_pq_cache[DISKANN_CTX.cache_size].cache = DiskAnnPQCache(pivots);
            ++DISKANN_CTX.cache_size;
            return true;
        } else {
            DISKANN_LRU_CACHE_PTR =
                New (DISKANN_CTX.ctx) lru_cache_type(DISKANN_CTX.ctx, size_t(DISKANN_CTX.max_cache_size));
            for (int i = 0; i < DISKANN_CTX.cache_size; ++i) {
                DISKANN_LRU_CACHE->put(DISKANN_CTX.diskann_pq_cache[i].oid, DISKANN_CTX.diskann_pq_cache[i].cache);
            }
        }
    } else if (DISKANN_LRU_CACHE->contains(index_oid)) {
        return false;
    }
    DISKANN_LRU_CACHE->put(index_oid, DiskAnnPQCache(pivots));
    return true;
}

bool diskann_clear_pq_cache(Oid index_oid)
{
    if (DISKANN_LRU_CACHE) {
        return DISKANN_LRU_CACHE->erase(index_oid);
    }
    for (int i = 0; i < DISKANN_CTX.cache_size; ++i) {
        auto &entry = DISKANN_CTX.diskann_pq_cache[i];
        if (entry.oid == index_oid) {
            entry.cache.destroy();
            errno_t rc = memmove_s(&entry, (MAX_ARR_CACHE_SIZE - i) * sizeof(DiskAnnPQCacheEntry),
                                    &entry + 1, (MAX_ARR_CACHE_SIZE - i - 1) * sizeof(DiskAnnPQCacheEntry));
            securec_check(rc, "\0", "\0");
            --DISKANN_CTX.cache_size;
            return true;
        }
    }
    return false;
}

void diskann_resize_cache(int new_size)
{
    if (!DISKANN_LRU_CACHE) {
        if (new_size < DISKANN_CTX.cache_size) {
            for (int i = new_size; i < DISKANN_CTX.cache_size; ++i) {
                DISKANN_CTX.diskann_pq_cache[i].cache.destroy();
            }
            DISKANN_CTX.cache_size = new_size;
        }
    } else if (size_t(new_size) < MAX_ARR_CACHE_SIZE) {
        int offset = int(DISKANN_LRU_CACHE->size() - new_size);
        int counter = 0;
        for (auto &pair : *DISKANN_LRU_CACHE) {
            if (counter >= offset) {
                auto &cache_entry = DISKANN_CTX.diskann_pq_cache[new_size - (counter - offset) - 1];
                cache_entry.oid = pair.key;
                cache_entry.cache = std::move(pair.value);
            }
            ++counter;
        }
        DISKANN_LRU_CACHE->destroy();
        DISKANN_LRU_CACHE_PTR = NULL;
    } else {
        DISKANN_LRU_CACHE->set_capacity(new_size);
    }
}

void DiskAnnInitPage(Buffer buf, Page page)
{
    PageInit(page, BufferGetPageSize(buf), sizeof(DiskAnnPageOpaque));
    DiskAnnPageGetOpaque(page)->page_id = DISKANN_PAGE_ID;
}

void DiskAnnInitMeta(Buffer buf, Page page)
{
    PageInit(page, BufferGetPageSize(buf), sizeof(DiskAnnPageOpaque));
    DiskAnnPageGetOpaque(page)->page_id = DISKANN_META_ID;
}

/*
 * Get proc info
 */
FmgrInfo *DiskAnnOptionalProcInfo(Relation rel, uint16 procnum)
{
    if (!OidIsValid(index_getprocid(rel, 1, procnum))) {
        return NULL;
    }

    return index_getprocinfo(rel, 1, procnum);
}

bool DiskAnnUseBTree(Relation index)
{
    return strcmp(NameStr(index->rd_am->amname), DEFAULT_HYBRIDANN_INDEX_TYPE) == 0;
}

float DiskAnnGetOcclusionFactor(Relation index)
{
    DiskAnnOptions *opts = (DiskAnnOptions *) index->rd_options;
    return opts != NULL ? opts->occlusion_factor : ALPHA;
}

uint32 DiskAnnGetMaxGraphDegree(Relation index)
{
    DiskAnnOptions *opts = (DiskAnnOptions *) index->rd_options;
    return opts != NULL ? opts->m : MAX_ANN_GRAPH_DEGREE;
}

uint32 DiskAnnGetBuildListSize(Relation index)
{
    DiskAnnOptions *opts = (DiskAnnOptions *) index->rd_options;
    return opts != NULL ? opts->ef_construction : DEFAULT_ANN_QUEUE_SIZE;
}

uint32 DiskAnnGetNumParallel(Relation index)
{
    DiskAnnOptions *opts = (DiskAnnOptions *) index->rd_options;
    return opts != NULL ? opts->parallel_workers : 0;
}

float DiskAnnGetSubGraphAdaptiveRelaxation(Relation index)
{
    DiskAnnOptions *opts = (DiskAnnOptions *) index->rd_options;
    return opts != NULL ? opts->adaptive_relaxation : 0.4;
}

float DiskAnnGetSubGraphMaxVectorsFactor(Relation index)
{
    DiskAnnOptions *opts = (DiskAnnOptions *) index->rd_options;
    return opts != NULL ? opts->subgraph_max_vectors_factor : 1.5;
}

