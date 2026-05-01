/**
 * Copyright ...
 * BM25 document storage
 */

#ifndef BM25_DOC_STORE_H
#define BM25_DOC_STORE_H

#include <algorithm>    /* max */
#include <boost/preprocessor/repetition/repeat_from_to.hpp>
#include <boost/preprocessor/arithmetic/add.hpp>

#include <vtl/disk_container/disk_hashtable.hpp>

#include "c.h"
#include "access/bm25/bm25_struct.h"
#include "access/bm25/bm25_utils.h"
#include "utils/relcache.h"
#include "storage/buf/buf.h"
#include "storage/item/itemptr.h"
#include "access/annvector/module/leak_checker.h"
#include "access/hash.h"

namespace bm25 {
struct DocumentStoreMeta {
    BlockNumber start_blkno;
};

struct DocIDHasher {
    uint32 operator()(const uint64 &val) const noexcept
    {
        uint32 lohalf = (uint32)val;
        uint32 hihalf = (uint32)((uint64)val >> 32);
        return DatumGetUInt32(hash_uint32(lohalf ^ hihalf));
    }
};

class DocumentStore : public ann_helper::LeakChecker {
#define INFO_DECL(z, i, x) using info_table##i = disk_container::DiskHashTable<uint64, DocInfo<i>, DocIDHasher>;
#define INFO2_DECL(z, i, x) using oid_info_table##i = disk_container::DiskHashTable<uint64, DocOidInfo<i>, DocIDHasher>;
    BOOST_PP_REPEAT_FROM_TO(1, BOOST_PP_ADD(BM25_MAX_NATTR, 1), INFO_DECL, x);
    BOOST_PP_REPEAT_FROM_TO(1, BOOST_PP_ADD(BM25_MAX_NATTR, 1), INFO2_DECL, x);
#undef INFO_DECL
#undef INFO2_DECL
public:
    static void create_doc_store(Relation rel, DocumentStoreMeta &meta, uint16 nattr, bool need_wal);
    DocumentStore(Relation rel, const DocumentStoreMeta &meta, uint16 nattr, bool need_wal);

    void destroy()
    {
        ann_helper::LeakChecker::destroy();
        helper([](auto &doc_infos) {
            doc_infos.destroy();
        });
    }

    void store(uint64 doc_id, const DocumentStats *stats, const ItemPointerData &tid, Oid part_id);
    bool get(uint64 doc_id, DocumentStats *stats, ItemPointer tid, Oid &part_id);

    bool erase(uint64 doc_id)
    {
        bool res;
        helper([&res, doc_id](auto &doc_infos) -> void {
            res = doc_infos.erase(doc_id);
        });
        return res;
    }

    doc_id_track vacuum(IndexBulkDeleteCallback callback, void *callback_state,
                        double *total_removed_length, uint32 *total_removed_doc,
                        IndexBulkDeleteResult *stats);

    auto get_stats()
    {
        disk_container::DiskHashTableStats stats;
        helper([&stats](auto &doc_infos) -> void {
            stats = doc_infos.get_stats();
        });
        return stats;
    }
    size_t base_data_size() const;

private:
    bool is_global;
    uint16 nattr;
#define ATTR_HELPER1(z, i, x) alignas(info_table##i) alignas(oid_info_table##i)
#define ATTR_HELPER2(z, i, x) sizeof(info_table##i), sizeof(oid_info_table##i),
    union {
        char unused;
        BOOST_PP_REPEAT_FROM_TO(1, BOOST_PP_ADD(BM25_MAX_NATTR, 1), ATTR_HELPER1, x)
        char _doc_infos_storage[std::max({
            BOOST_PP_REPEAT_FROM_TO(1, BOOST_PP_ADD(BM25_MAX_NATTR, 1), ATTR_HELPER2, x)
        })];
    };  /* 1. make sure storage gets aligned; 2. avoid init char[] */
#undef ATTR_HELPER1
#undef ATTR_HELPER2

    template <class F>
    void helper(F &&f)
    {
#define HELPER_CASE(z, i, x) case i:   \
    if (is_global) {    \
        std::forward<F>(f)(reinterpret_cast<oid_info_table##i &>(_doc_infos_storage));  \
    } else {    \
        std::forward<F>(f)(reinterpret_cast<info_table##i &>(_doc_infos_storage));  \
    } break;
        switch (nattr) {
            BOOST_PP_REPEAT_FROM_TO(1, BOOST_PP_ADD(BM25_MAX_NATTR, 1), HELPER_CASE, x);
            default:
                ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
                    errmsg("Unhandled number of indexed columns: %u", nattr)));
        }
    }
#undef HELPER_CASE
};
} /* namespace bm25 */

#endif /* BM25_DOC_STORE_H */
