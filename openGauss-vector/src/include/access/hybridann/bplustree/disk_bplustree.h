/**
 * Copyright ...
 * Combining B+ tree with PG interface and hibrid vector index.
 */

#ifndef DISKANN_CONTAINER_DISK_BPLUSTREE_H
#define DISKANN_CONTAINER_DISK_BPLUSTREE_H

#include <boost/preprocessor/repetition/repeat_from_to.hpp>

#include "access/annvector/macro.h"
#include "access/hybridann/bplustree/bplustree_search.h"
#include "access/hybridann/bplustree/bplustree_build.h"
#include "access/hybridann/bplustree/bplustree_vector.h"
#include "access/hybridann/bplustree/bplustree_vacuum.h"

namespace disk_container {
class DiskBPlusTree : public SearchBPlusTree, public BuildBPlusTree, public VectorBPlusTree, public VacuumBPlusTree {
public:
    using SearchBPlusTree::SearchBPlusTree;
    using BuildBPlusTree::BuildBPlusTree;
    using VectorBPlusTree::VectorBPlusTree;
};

inline BlockNumber create_disk_btree_metapage(Relation index, bool is_fixed, bool need_wal)
{
    LockRelationForExtension(index, ExclusiveLock);
    Buffer meta_buf = ReadBuffer(index, P_NEW);
    UnlockRelationForExtension(index, ExclusiveLock);
    Page meta_page = BufferGetPage(meta_buf);

    /* init meta */
    PageInit(meta_page, BLCKSZ, 0); /* no opaque for meta page */
    DiskBTMeta meta = (DiskBTMeta)PageGetContents(meta_page);
    meta->magic = DISK_BT_META_MAGIC;
    meta->version = 1u;
    meta->unlinked_pages_metablkno = FreeSpace<BlockNumber>::get_freespace_meta(index, false);
    ((PageHeader) meta_page)->pd_lower = 
        ((char *)meta + sizeof(DiskBTMetaData)) - (char *)meta_page;

    BlockNumber meta_blkno = BufferGetBlockNumber(meta_buf);
    PageSetChecksumInplace(meta_page, meta_blkno);
    MarkBufferDirty(meta_buf);
    ReleaseBuffer(meta_buf);

    return meta_blkno;
}

// template <uint32 N> using FixedDiskBPlusTree = BPlusTreeImpl<N>;
} /* namespace disk_container */

namespace ann_helper {
template <template <uint32 N> class F>
inline void call_template_with_number(void *args, uint32 n)
{
#define DISKANN_GET_TEMPLATE_WITH_NUMBER(z, n, x) case n: { F<n>::call(args); } break;
    switch (n) {
        BOOST_PP_REPEAT_FROM_TO(1, 11, DISKANN_GET_TEMPLATE_WITH_NUMBER, ~)
        default:
            elog(ERROR, "unsupported fixed length %u (valid range: [%u, %u])", n, 1u, 10u);
            __builtin_unreachable();
    }
#undef DISKANN_GET_TEMPLATE_WITH_NUMBER
}

} /* namespace ann_helper */

#endif /* DISKANN_CONTAINER_DISK_BPLUSTREE_H */
