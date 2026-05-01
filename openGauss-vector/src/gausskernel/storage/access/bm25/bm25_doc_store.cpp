/**
 * Copyright ...
 */

#include "access/bm25/bm25_doc_store.h"
#include "access/bm25/bm25.h"

using namespace bm25;

#define HELPER_CASE(z, i, func) case i: func(i); break;

#define HELPER(func) do { switch(nattr) {                           \
    BOOST_PP_REPEAT_FROM_TO(1, BOOST_PP_ADD(BM25_MAX_NATTR, 1), HELPER_CASE, func)  \
    default:                                                        \
        ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),            \
            errmsg("Unhandled number of indexed columns: %u", nattr)));   \
}} while(0)

void DocumentStore::create_doc_store(Relation rel, DocumentStoreMeta &meta, uint16 nattr,
                                     bool need_wal)
{
#define INTERNAL_FUNC(n) meta.start_blkno = info_table##n::get_disk_hashtable(rel, need_wal)
#define INTERNAL2_FUNC(n) meta.start_blkno = info_table##n::get_disk_hashtable(rel, need_wal)
    if (RelationIsGlobalIndex(rel)) {
        HELPER(INTERNAL2_FUNC);
    } else {
        HELPER(INTERNAL_FUNC);
    }
#undef INTERNAL_FUNC
#undef INTERNAL2_FUNC
}

DocumentStore::DocumentStore(Relation rel, const DocumentStoreMeta &meta, uint16 nattr,
                             bool need_wal)
    : is_global(RelationIsGlobalIndex(rel)), nattr(nattr)
{
#define INTERNAL_FUNC(n) new (_doc_infos_storage) info_table##n(rel, meta.start_blkno, need_wal)
#define INTERNAL2_FUNC(n) new (_doc_infos_storage) oid_info_table##n(rel, meta.start_blkno, need_wal)
    if (is_global) {
        HELPER(INTERNAL2_FUNC);
    } else {
        HELPER(INTERNAL_FUNC);
    }
#undef INTERNAL_FUNC
#undef INTERNAL2_FUNC
}

void DocumentStore::store(uint64 doc_id, const DocumentStats *stats, const ItemPointerData &tid,
                          Oid part_id)
{
#define INTERNAL_FUNC(n) if (is_global) {   \
        DocOidInfo<n> dinfo;       \
        dinfo.tid = tid;        \
        for (uint16 i = 0; i < n; ++i) {    \
            dinfo.stats[i] = stats[i];      \
        }                       \
        dinfo.part_id = part_id;\
        reinterpret_cast<oid_info_table##n &>(_doc_infos_storage).insert(doc_id, dinfo);    \
    } else {    \
        DocInfo<n> dinfo;       \
        dinfo.tid = tid;        \
        for (uint16 i = 0; i < n; ++i) {    \
            dinfo.stats[i] = stats[i];      \
        }                       \
        reinterpret_cast<info_table##n &>(_doc_infos_storage).insert(doc_id, dinfo);    \
    }
    HELPER(INTERNAL_FUNC);
#undef INTERNAL_FUNC
}

bool DocumentStore::get(uint64 doc_id, DocumentStats *stats, ItemPointer tid, Oid &part_id)
{
#define INTERNAL_FUNC(n) if (is_global) {   \
        DocOidInfo<n> dinfo;       \
        if (!reinterpret_cast<oid_info_table##n &>(_doc_infos_storage).get(doc_id, dinfo)) {    \
            return false;       \
        }                       \
        for (uint16 i = 0; i < n; ++i) {        \
            stats[i] = dinfo.stats[i];          \
        }                       \
        ItemPointerCopy(&dinfo.tid, tid);       \
        part_id = dinfo.part_id;\
        return true;            \
    } else {    \
        DocInfo<n> dinfo;       \
        if (!reinterpret_cast<info_table##n &>(_doc_infos_storage).get(doc_id, dinfo)) {    \
            return false;       \
        }                       \
        for (uint16 i = 0; i < n; ++i) {        \
            stats[i] = dinfo.stats[i];          \
        }                       \
        ItemPointerCopy(&dinfo.tid, tid);       \
        return true;            \
    }
    HELPER(INTERNAL_FUNC);
#undef INTERNAL_FUNC
    return false; /* make compiler happy */
}

doc_id_track DocumentStore::vacuum(IndexBulkDeleteCallback callback, void *callback_state,
                                   double *total_removed_length, uint32 *total_removed_doc,
                                   IndexBulkDeleteResult *stats)
{
    doc_id_track res(500'000ul);
    const auto get_deleted_id = [&](const auto &kv) -> bool {
        if (!callback(const_cast<ItemPointer>(&kv.v.tid), callback_state,
                      kv.v.get_part_id(), InvalidBktId)) {
            return false;
        }
        const uint64 doc_id = kv.k;
        res.insert(doc_id);
        uint32 counter = 0;
        for (const auto &s : kv.v.stats) {
            if (s.length > 0) {
                total_removed_length[counter] += s.length;
                ++total_removed_doc[counter];
            }
            ++counter;
        }
        return true;
    };
#define INTERNAL_FUNC(n) reinterpret_cast<info_table##n &>(_doc_infos_storage).erase_cif(get_deleted_id)
#define INTERNAL2_FUNC(n) reinterpret_cast<oid_info_table##n &>(_doc_infos_storage).erase_cif(get_deleted_id)
    if (is_global) {
        HELPER(INTERNAL2_FUNC);
    } else {
        HELPER(INTERNAL_FUNC);
    }
#undef INTERNAL_FUNC
#undef INTERNAL2_FUNC
    return res;
}

size_t DocumentStore::base_data_size() const
{
#define INTERNAL_FUNC(n) return sizeof(info_table##n::kv_base)
#define INTERNAL2_FUNC(n) return sizeof(oid_info_table##n::kv_base)
    if (is_global) {
        HELPER(INTERNAL2_FUNC);
    } else {
        HELPER(INTERNAL_FUNC);
    }
#undef INTERNAL_FUNC
#undef INTERNAL2_FUNC
    return 0;   /* make compiler happy */
}

#undef HELPER_CASE
