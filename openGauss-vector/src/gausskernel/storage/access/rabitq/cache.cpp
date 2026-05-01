/**
 * Copyright ...
 * RaBitQ LRU Cache
 */

#include <vtl/lrucache.hpp>
#include "postgres.h"
#include "knl/knl_variable.h"
#include "access/rabitq/rabitq_cache.h"

using lru_cache_type = vtl::LRUCache<Oid, rabitq::RaBitQCache>;
#define RABITQ_LRU_CACHE_PTR (RABITQ_CTX.lru_cache)
#define RABITQ_LRU_CACHE ((lru_cache_type *)RABITQ_LRU_CACHE_PTR)

namespace rabitq {

void RaBitQCache::destroy()
{
    oid = InvalidOid;
    RABITQ_CACHE_FREE_EXT(fixed_data);
}

const RaBitQCache *get_rabitq_cache(Oid index_oid)
{
    if (!RABITQ_LRU_CACHE) {
        for (int i = 0; i < RABITQ_CTX.cache_size; ++i) {
            if (RABITQ_CTX.caches[i].oid == index_oid) {
                return &RABITQ_CTX.caches[i];
            }
        }
        return NULL;
    }
    return RABITQ_LRU_CACHE->get(index_oid);
}

/* return false if cache already exists.
 * rabitq cached data will never change,
 * for index rebuilding, call clear cache before resetting. */
bool set_rabitq_cache(RaBitQCache cache)
{
    if (!RABITQ_LRU_CACHE) {
        for (int i = 0; i < RABITQ_CTX.cache_size; ++i) {
            if (RABITQ_CTX.caches[i].oid == cache.oid) {
                return false;
            }
        }
        if (RABITQ_CTX.cache_size < RABITQ_MAX_CACHE_SIZE) {
            RABITQ_CTX.caches[RABITQ_CTX.cache_size] = cache;
            ++RABITQ_CTX.cache_size;
            return true;
        } else {
            RABITQ_LRU_CACHE_PTR =
                New (RABITQ_CTX.cache_ctx) lru_cache_type(RABITQ_CTX.cache_ctx, size_t(RABITQ_LRU_CACHE_SIZE));
            for (int i = 0; i < RABITQ_CTX.cache_size; ++i) {
                if (RABITQ_CTX.caches[i].oid != InvalidOid) {
                    RABITQ_LRU_CACHE->put(RABITQ_CTX.caches[i].oid, RABITQ_CTX.caches[i]);
                }
            }
        }
    } else if (RABITQ_LRU_CACHE->contains(cache.oid)) {
        return false;
    }
    RABITQ_LRU_CACHE->put(cache.oid, cache);
    return true;
}

bool clear_rabitq_cache(Oid index_oid)
{
    if (RABITQ_LRU_CACHE) {
        return RABITQ_LRU_CACHE->erase(index_oid);
    }
    for (int i = 0; i < RABITQ_CTX.cache_size; ++i) {
        auto &entry = RABITQ_CTX.caches[i];
        if (entry.oid == index_oid) {
            entry.destroy();
            errno_t rc = memmove_s(&entry, (RABITQ_MAX_CACHE_SIZE - i) * sizeof(RaBitQCache),
                                    &entry + 1, (RABITQ_MAX_CACHE_SIZE - i - 1) * sizeof(RaBitQCache));
            securec_check(rc, "\0", "\0");
            --RABITQ_CTX.cache_size;
            return true;
        }
    }
    return false;
}

void resize_rabitq_cache(int new_size)
{
    if (!RABITQ_LRU_CACHE) {
        if (new_size < RABITQ_CTX.cache_size) {
            for (int i = new_size; i < RABITQ_CTX.cache_size; ++i) {
                RABITQ_CTX.caches[i].destroy();
            }
            RABITQ_CTX.cache_size = new_size;
        }
    } else if (new_size < RABITQ_MAX_CACHE_SIZE) {
        int offset = int(RABITQ_LRU_CACHE->size() - new_size);
        int counter = 0;
        for (auto &pair : *RABITQ_LRU_CACHE) {
            if (counter >= offset) {
                auto &cache_entry = RABITQ_CTX.caches[new_size - (counter - offset) - 1];
                cache_entry= std::move(pair.value);
            }
            ++counter;
        }
        RABITQ_LRU_CACHE->destroy();
        RABITQ_LRU_CACHE_PTR = NULL;
    } else {
        RABITQ_LRU_CACHE->set_capacity(new_size);
    }
}

} /* namespace rabitq */
