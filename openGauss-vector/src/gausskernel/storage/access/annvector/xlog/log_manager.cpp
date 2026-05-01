/**
 * Copyright ...
 */

#include <vtl/disk_container/diskvector.hpp>

#include "access/annvector/xlog/log_manager.h"
#include "access/diskann/vector_bt.h"
#include "access/xlog.h"
#include "access/diskann/diskann.h"
#include "access/annvector/store/vector_smgr.h"
#include "storage/smgr/segment_internal.h"
#include "access/hybridann/bplustree/disk_impl.h"

using namespace disk_container;

void LogManager::log_build_index(ForkNumber fork_number, bool use_btree)
{
    BlockNumber number_of_blocks = RelationGetNumberOfBlocksInFork(_rel, fork_number);
    RmgrId rmid = use_btree ? RM_HYBRIDBT_ID : RM_DISKANN_ID;
    uint8 info = (fork_number == MAIN_FORKNUM) ?
        (use_btree ? XLOG_HYBRID_BUILD_INDEX : XLOG_DISKANN_BUILD_INDEX) :
        (use_btree ? XLOG_HYBRID_UNLOG_BUILD_INDEX : XLOG_DISKANN_UNLOG_BUILD_INDEX);

    log_newpage_range(_rel, fork_number, 0, number_of_blocks, true, rmid, info, NULL, true);
}

void LogManager::diskann_xlog_add_elem(PageData &page_data, char *elem_data, size_t elem_size,
                                       uint32 offset)
{
    xl_diskann_add_elem elem;
    elem.elem_size = elem_size;
    elem.offset = offset;

    XLogBeginInsert();
    START_CRIT_SECTION();
    XLogRegisterData((char *)&elem, sizeof(xl_diskann_add_elem));
    XLogRegisterBuffer(0, page_data.buf, REGBUF_STANDARD);
    XLogRegisterBufData(0, elem_data, elem_size);
    const XLogRecPtr rec_ptr = XLogInsert(RM_DISKANN_ID, XLOG_DISKANN_SET_ELEM);
    PageSetLSN(BufferGetPage(page_data.buf), rec_ptr);
    END_CRIT_SECTION();
}

void LogManager::diskann_extend_newpages(BlockNumber start_blkno, BlockNumber end_blkno)
{
    log_newpage_range(_rel, MAIN_FORKNUM, start_blkno, end_blkno, true, RM_DISKANN_ID,
                      XLOG_DISKANN_EXTEND_NEWPAGES);
}

void LogManager::diskann_update_meta_start_npages(PageData &meta_buf, uint32 num_pages,
                                                  BlockNumber start_blkno)
{
    xl_diskann_update_start_npages xl_rec;
    xl_rec.num_pages = num_pages;
    xl_rec.start_blkno = start_blkno;

    XLogBeginInsert();
    START_CRIT_SECTION();
    XLogRegisterBuffer(0, meta_buf.buf, REGBUF_STANDARD);
    XLogRegisterBufData(0, (char *)&xl_rec, sizeof(xl_diskann_update_start_npages));
    XLogRecPtr rec_ptr = XLogInsert(RM_DISKANN_ID, XLOG_DISKANN_UPDATE_META_START_NPAGES);
    PageSetLSN(BufferGetPage(meta_buf.buf), rec_ptr);
    END_CRIT_SECTION();
}

void LogManager::diskann_update_meta_nitem(PageData &meta_buf, size_t nitem)
{
    XLogBeginInsert();
    START_CRIT_SECTION();
    XLogRegisterBuffer(0, meta_buf.buf, REGBUF_STANDARD);
    XLogRegisterBufData(0, (char *)&nitem, sizeof(size_t));
    XLogRecPtr rec_ptr = XLogInsert(RM_DISKANN_ID, XLOG_DISKANN_UPDATE_META_NITEM);
    PageSetLSN(BufferGetPage(meta_buf.buf), rec_ptr);
    END_CRIT_SECTION();
}

void LogManager::diskann_inplace_filter_add_data(Buffer buf, Page page, uint32 offset, size_t data_size)
{
    xl_diskann_inplace_filter_add_data xl_rec;
    xl_rec.offset = offset;
    xl_rec.data_size = data_size;

    XLogBeginInsert();
    START_CRIT_SECTION();
    XLogRegisterBuffer(0, buf, REGBUF_STANDARD);
    XLogRegisterData((char *)&xl_rec, sizeof(xl_diskann_inplace_filter_add_data));
    XLogRegisterBufData(0, (char *)(page + xl_rec.offset), xl_rec.data_size);
    XLogRecPtr rec_ptr = XLogInsert(RM_DISKANN_ID, XLOG_DISKANN_INPLACE_FILTER_ADD_DATA);
    PageSetLSN(page, rec_ptr);
    END_CRIT_SECTION();
}

void LogManager::diskann_inplace_filter_add_item(Buffer buf, Page page, char *item, size_t size, OffsetNumber offset, bool isOverWrite)
{
    xl_diskann_inplace_filter_add_item xl_rec;
    xl_rec.size = size;
    xl_rec.offset = offset;
    xl_rec.isOverWrite = isOverWrite;

    XLogBeginInsert();
    START_CRIT_SECTION();
    XLogRegisterBuffer(0, buf, REGBUF_STANDARD);
    XLogRegisterData((char *)&xl_rec, sizeof(xl_diskann_inplace_filter_add_item));
    XLogRegisterBufData(0, item, size);
    XLogRecPtr rec_ptr = XLogInsert(RM_DISKANN_ID, XLOG_DISKANN_INPLACE_FILTER_ADD_ITEM);
    PageSetLSN(page, rec_ptr);
    END_CRIT_SECTION();
}

void LogManager::diskann_inplace_filter_delete_item(Buffer buf, Page page, OffsetNumber offset)
{
    XLogBeginInsert();
    START_CRIT_SECTION();
    XLogRegisterBuffer(0, buf, REGBUF_STANDARD);
    XLogRegisterData((char *)&offset, sizeof(OffsetNumber));
    XLogRecPtr rec_ptr = XLogInsert(RM_DISKANN_ID, XLOG_DISKANN_INPLACE_FILTER_DELETE_ITEM);
    PageSetLSN(page, rec_ptr);
    END_CRIT_SECTION();
}

void LogManager::diskann_inplace_filter_multi_delete(Buffer buf, Page page, OffsetNumber *offsets, size_t n)
{
    XLogBeginInsert();
    START_CRIT_SECTION();
    XLogRegisterBuffer(0, buf, REGBUF_STANDARD);
    XLogRegisterData((char *)&n, sizeof(size_t));
    XLogRegisterBufData(0, (char *)offsets, n * sizeof(OffsetNumber));
    XLogRecPtr rec_ptr = XLogInsert(RM_DISKANN_ID, XLOG_DISKANN_INPLACE_FILTER_MULTI_DELETE);
    PageSetLSN(page, rec_ptr);
    END_CRIT_SECTION();
}

static void log_vector(off_t offset, int nbytes, char *vec, RmgrId rmid, uint8 info, Relation rel)
{
    xl_ann_add_vector xl_rec;
    RelFileNodeRelCopy(xl_rec.r_Node, rel->rd_node);
    xl_rec.offset = offset;
    xl_rec.nbytes = nbytes;

    XLogBeginInsert();
    START_CRIT_SECTION();
    XLogRegisterData((char *)&xl_rec, sizeof(xl_ann_add_vector));
    XLogRegisterData(vec, nbytes);
    XLogInsert(rmid, info);
    END_CRIT_SECTION();
}

void LogManager::log_write_vector(off_t offset, int nbytes, char *vec, bool use_btree)
{
    RmgrId rmid = use_btree ? RM_HYBRIDBT_ID : RM_DISKANN_ID;
    uint8 info = use_btree ? XLOG_HYBRID_ADD_VECTOR : XLOG_DISKANN_ADD_VECTOR;
    log_vector(offset, nbytes, vec, rmid, info, _rel);
}

void LogManager::log_build_vector(Relation heap, Relation index, uint32 dim, size_t size, bool use_btree)
{
    if (!heap || size == 0) {
        return;
    }
    size_t vector_size = use_btree ? size : size + 1;
    constexpr size_t wal_block_size = 25'600'000ul;
    const size_t block_size = wal_block_size / dim;

    char *buf = (char *)palloc(block_size * dim * sizeof(float));

    for (size_t i = 0; i < vector_size; i += block_size) {
        size_t block = std::min(vector_size - i, block_size);
        vec_read(index->rd_smgr, i * dim * sizeof(float), block * dim * sizeof(float), buf);
        log_write_vector(i * dim * sizeof(float), block * dim * sizeof(float), buf, use_btree);
    }
    pfree(buf);
}

void LogManager::log_vecindex_set_temp_unlink(Buffer buffer, Page page, bool isUnlink)
{
    XLogBeginInsert();
    START_CRIT_SECTION();
    XLogRegisterBuffer(0, buffer, REGBUF_STANDARD);
    XLogRegisterBufData(0, (char *)&isUnlink, sizeof(bool));
    XLogRecPtr recptr = XLogInsert(RM_VECTORINDEX_ID, XLOG_VECTORINDEX_SET_TEMP_UNLINK);
    PageSetLSN(page, recptr);
    END_CRIT_SECTION();
}

void LogManager::log_vecindex_set_operation(Buffer buffer, bool idle)
{
    XLogRecPtr recptr;
    XLogBeginInsert();
    START_CRIT_SECTION();
    XLogRegisterBuffer(0, buffer, REGBUF_STANDARD);
    XLogRegisterBufData(0, (char *)&idle, sizeof(bool));
    recptr = XLogInsert(RM_VECTORINDEX_ID,
        idle ? XLOG_VECTORINDEX_FINISH_OPERATION : XLOG_VECTORINDEX_START_OPERATION);
    PageSetLSN(BufferGetPage(buffer), recptr);
    END_CRIT_SECTION();
}

void LogManager::log_vecindex_meta_and_node(BlockNumber new_meta_blkno)
{
    Buffer new_meta_buf = ReadBuffer(_rel, new_meta_blkno);
    LockBuffer(new_meta_buf, BUFFER_LOCK_EXCLUSIVE);

    VectorIndexMeta new_meta = (VectorIndexMeta)PageGetContents(BufferGetPage(new_meta_buf));
    Buffer new_node_buf = ReadBuffer(_rel, new_meta->node_blkno);
    LockBuffer(new_node_buf, BUFFER_LOCK_EXCLUSIVE);
    XLogBeginInsert();
    START_CRIT_SECTION();
    MarkBufferDirty(new_meta_buf);
    MarkBufferDirty(new_node_buf);
    XLogRegisterBuffer(0, new_meta_buf, REGBUF_FORCE_IMAGE | REGBUF_STANDARD);
    XLogRegisterBuffer(1, new_node_buf, REGBUF_FORCE_IMAGE | REGBUF_STANDARD);
    XLogRecPtr recptr = XLogInsert(RM_VECTORINDEX_ID, XLOG_VECTORINDEX_META_AND_NODE);
    PageSetLSN(BufferGetPage(new_meta_buf), recptr);
    PageSetLSN(BufferGetPage(new_node_buf), recptr);
    END_CRIT_SECTION();
    UnlockReleaseBuffer(new_meta_buf);
    UnlockReleaseBuffer(new_node_buf);
}

void LogManager::log_vecindex_set_index_ptr(Buffer buffer, Page page, size_t level,
                                            BlockNumber blkno, bool indexPtrIsNew)
{
    XLogBeginInsert();
    START_CRIT_SECTION();
    XLogRegisterData((char *)&indexPtrIsNew, sizeof(bool));
    XLogRegisterBuffer(0, buffer, REGBUF_STANDARD);
    XLogRegisterBufData(0, (char *)&level, sizeof(size_t));
    XLogRegisterBufData(0, (char *)&blkno, sizeof(BlockNumber));
    XLogRecPtr recptr = XLogInsert(RM_VECTORINDEX_ID, XLOG_VECTORINDEX_SET_INDEX_PTR);
    PageSetLSN(page, recptr);
    END_CRIT_SECTION();
}

void LogManager::
log_invalidate_vector_cache(Relation rel, size_t loc, size_t elem_size, RmgrId rmid,
                                             uint8 info)
{
    xl_invalidate_vector_cache xl_rec;
    RelFileNodeRelCopy(xl_rec.r_Node, rel->rd_node);
    xl_rec.relNode = rel->rd_smgr->smgr_rnode.node.relNode;
    xl_rec.loc = loc;
    xl_rec.elem_size = elem_size;

    XLogBeginInsert();
    START_CRIT_SECTION();
    XLogRegisterData((char *)&xl_rec, sizeof(xl_invalidate_vector_cache));
    XLogInsert(rmid, info);
    END_CRIT_SECTION();
}

void hybrid_redo_build_index(XLogReaderState *record)
{
    uint8 blockId;

    for (blockId = 0; blockId <= XLogRecMaxBlockId(record); blockId++) {
        RedoBufferInfo buffer;
        if (XLogReadBufferForRedo(record, blockId, &buffer) != BLK_RESTORED) {
            elog(ERROR, "hybrid record did not contain a full-image of a build index page");
        }
        if (BufferIsValid(buffer.buf)) {
            UnlockReleaseBuffer(buffer.buf);
        }
    }
}

void hybrid_redo_unlog_build_index(XLogReaderState *record) { hybrid_redo_build_index(record); }

void hybrid_redo_write_vec(XLogReaderState *record)
{
    char *data = XLogRecGetData(record);
    xl_ann_add_vector *xl_rec = (xl_ann_add_vector *)data;
    char *vec = (char *)(data + sizeof(xl_ann_add_vector));
    RelFileNode tmp_node;
    RelFileNodeCopy(tmp_node, xl_rec->r_Node, XLogRecGetBucketId(record));
    SMgrRelation smgr = smgropen(tmp_node, InvalidBackendId);

    /* If vector file does not exist, skip it. */
    if (!smgr->md_fd[VECTOR_FORKNUM] && !smgrexists(smgr, VECTOR_FORKNUM)) {
        return;
    }

    vec_write(smgr, xl_rec->offset, xl_rec->nbytes, vec, false);
}

void hybrid_redo_bt_insert_data(XLogReaderState *record)
{
    XLogRecPtr lsn = record->EndRecPtr;
    RedoBufferInfo l_buffer;
    OffsetNumber *newitemoff = (OffsetNumber *)XLogRecGetData(record);

    if (XLogReadBufferForRedo(record, 0, &l_buffer) == BLK_NEEDS_REDO) {
        char *data = XLogRecGetBlockData(record, 0, NULL);
        DiskNodeImpl lnode(l_buffer.buf);
        lnode.insert(*newitemoff, (BTTupleData *)data);
        PageSetLSN(l_buffer.pageinfo.page, lsn);
        lnode.mark_dirty();
    }
    if (XLogRecHasBlockRef(record, 1)) {
        RedoBufferInfo c_buffer;
        if (XLogReadBufferForRedo(record, 1, &c_buffer) == BLK_NEEDS_REDO) {
            DiskNodeImpl cnode(c_buffer.buf);
            cnode.set_incomplete_split(false);
            PageSetLSN(c_buffer.pageinfo.page, lsn);
            cnode.mark_dirty();
        }
        if (BufferIsValid(c_buffer.buf)) {
            UnlockReleaseBuffer(c_buffer.buf);
        }
    }
    if (BufferIsValid(l_buffer.buf)) {
        UnlockReleaseBuffer(l_buffer.buf);
    }
}

void hybrid_redo_bt_split(XLogReaderState *record)
{
    XLogRecPtr lsn = record->EndRecPtr;
    RedoBufferInfo left_buffer, r_buffer;

    xl_hybrid_split *xl_rec = (xl_hybrid_split *)XLogRecGetData(record);

    if (XLogReadBufferForRedo(record, 1, &r_buffer) != BLK_RESTORED) {
        elog(ERROR, "Hybrid split record did not contain a full-image of the right node");
    }

    if (XLogReadBufferForRedo(record, 0, &left_buffer) == BLK_NEEDS_REDO) {
        char *data = XLogRecGetBlockData(record, 0, NULL);
        errno_t rc = memcpy_s(left_buffer.pageinfo.page, BLCKSZ, data, BLCKSZ);
        securec_check(rc, "\0", "\0");
        PageSetLSN(left_buffer.pageinfo.page, lsn);
        MarkBufferDirty(left_buffer.buf);
    }

    if (xl_rec->is_right_most) {
        RedoBufferInfo right_buffer;;
        if (XLogReadBufferForRedo(record, 2, &right_buffer) == BLK_NEEDS_REDO) {
            DiskNodeImpl right_node(right_buffer.buf);
            BlockNumber *blkno = (BlockNumber *)XLogRecGetBlockData(record, 2, NULL);
            right_node.prev() = *blkno;
            PageSetLSN(right_buffer.pageinfo.page, lsn);
            right_node.mark_dirty();
        }
        if (BufferIsValid(right_buffer.buf)) {
            UnlockReleaseBuffer(right_buffer.buf);
        }
    }

    if (!xl_rec->isleaf) {
        RedoBufferInfo child_buffer;
        if (XLogReadBufferForRedo(record, 3, &child_buffer) == BLK_NEEDS_REDO) {
            DiskNodeImpl child_node(child_buffer.buf);
            child_node.set_incomplete_split(false);
            PageSetLSN(child_buffer.pageinfo.page, lsn);
            child_node.mark_dirty();
        }
        if (BufferIsValid(child_buffer.buf)) {
            UnlockReleaseBuffer(child_buffer.buf);
        }
    }
    if (BufferIsValid(r_buffer.buf)) {
        UnlockReleaseBuffer(r_buffer.buf);
    }
    if (BufferIsValid(left_buffer.buf)) {
        UnlockReleaseBuffer(left_buffer.buf);
    }
}

void hybrid_redo_bt_split_root(XLogReaderState *record)
{
    XLogRecPtr lsn = record->EndRecPtr;
    RedoBufferInfo root_buffer, l_buffer, meta_buffer;

    if (XLogReadBufferForRedo(record, 0, &root_buffer) != BLK_RESTORED) {
        elog(ERROR, "Hybrid split root record did not contain a full-page image of root page");
    }

    if (XLogReadBufferForRedo(record, 1, &l_buffer) == BLK_NEEDS_REDO) {
        DiskNodeImpl l_node(l_buffer.buf);
        l_node.set_root(false);
        l_node.set_incomplete_split(false);
        PageSetLSN(l_buffer.pageinfo.page, lsn);
        l_node.mark_dirty();
    }

    if (XLogReadBufferForRedo(record, 2, &meta_buffer) == BLK_NEEDS_REDO) {
        BlockNumber *root_blkno = (BlockNumber *)XLogRecGetBlockData(record, 2, NULL);
        DiskBTMeta metapage =(DiskBTMeta) meta_buffer.pageinfo.page;
        metapage->root_ptr = *root_blkno;
        PageSetLSN(meta_buffer.pageinfo.page, lsn);
        MarkBufferDirty(meta_buffer.buf);
    }
    if (BufferIsValid(root_buffer.buf)) {
        UnlockReleaseBuffer(root_buffer.buf);
    }
    if (BufferIsValid(l_buffer.buf)) {
        UnlockReleaseBuffer(l_buffer.buf);
    }
    if (BufferIsValid(meta_buffer.buf)) {
        UnlockReleaseBuffer(meta_buffer.buf);
    }
}

void hybrid_redo_bt_vacuum(XLogReaderState *record)
{
    XLogRecPtr lsn = record->EndRecPtr;
    RedoBufferInfo buffer;
    int *num_deletable = (int *) XLogRecGetData(record);

    if (XLogReadBufferForRedo(record, 0, &buffer) == BLK_NEEDS_REDO) {
        Page page = buffer.pageinfo.page;
        char *deletable = XLogRecGetBlockData(record, 0, NULL);
        if (*num_deletable > 0) {
            PageIndexMultiDelete(page, (OffsetNumber *)deletable, *num_deletable);
        }
        DiskNodeImpl node(buffer.buf);
        node.cycle_id() = 0;
        node.set_has_garbage(false);
        PageSetLSN(page, lsn);
        node.mark_dirty();
    }
    if (BufferIsValid(buffer.buf)) {
        UnlockReleaseBuffer(buffer.buf);
    }
}

void hybrid_redo_bt_mark_page_halfdead(XLogReaderState *record)
{
    XLogRecPtr lsn = record->EndRecPtr;
    RedoBufferInfo topparent_buffer;
    RedoBufferInfo node_buffer;

    if (XLogReadBufferForRedo(record, 0, &topparent_buffer) == BLK_NEEDS_REDO) {
        xl_hybrid_mark_page_harfhead *data = (xl_hybrid_mark_page_harfhead *)XLogRecGetBlockData(record, 0, NULL);
        DiskNodeImpl topparent(topparent_buffer.buf);
        topparent.get_data(data->topoff)->set_ptr(data->rightsib);
        topparent.delete_tuple(OffsetNumberNext(data->topoff));
        PageSetLSN(topparent_buffer.pageinfo.page, lsn);
        topparent.mark_dirty();
    }

    if (XLogReadBufferForRedo(record, 1, &node_buffer) == BLK_NEEDS_REDO) {
        DiskNodeImpl node(node_buffer.buf);
        char *tupledata = XLogRecGetBlockData(record, 1, NULL);
        node.set_half_dead(true);
        node.delete_tuple(P_HIKEY);
        node.insert(P_HIKEY, (BTTupleData *)tupledata);
        PageSetLSN(node_buffer.pageinfo.page, lsn);
        node.mark_dirty();
    }
    if (BufferIsValid(topparent_buffer.buf)) {
        UnlockReleaseBuffer(topparent_buffer.buf);
    }
    if (BufferIsValid(node_buffer.buf)) {
        UnlockReleaseBuffer(node_buffer.buf);
    }
}

void hybrid_redo_bt_unlink_halfdead_page(XLogReaderState *record)
{
    XLogRecPtr lsn = record->EndRecPtr;
    RedoBufferInfo rnode_buffer, node_buffer;

    xl_hybrid_unlink_harfhead_page *xl_rec = (xl_hybrid_unlink_harfhead_page *)XLogRecGetData(record);

    if (XLogReadBufferForRedo(record, 0, &rnode_buffer) == BLK_NEEDS_REDO) {
        DiskNodeImpl rnode(rnode_buffer.buf);
        rnode.prev() = xl_rec->leftsib;
        PageSetLSN(rnode_buffer.pageinfo.page, lsn);
        rnode.mark_dirty();
    }

    if (XLogReadBufferForRedo(record, 1, &node_buffer) == BLK_NEEDS_REDO) {
        DiskNodeImpl node(node_buffer.buf);
        node.set_half_dead(false);
        node.set_deleted(true);
        node.set_xact(xl_rec->xid);
        PageSetLSN(node_buffer.pageinfo.page, lsn);
        node.mark_dirty();
    }

    if (XLogRecHasBlockRef(record, 2)) {
        RedoBufferInfo lnode_buffer;
        if (XLogReadBufferForRedo(record, 2, &lnode_buffer) == BLK_NEEDS_REDO) {
            DiskNodeImpl lnode(lnode_buffer.buf);
            lnode.next() = xl_rec->rightsib;
            PageSetLSN(lnode_buffer.pageinfo.page, lsn);
            lnode.mark_dirty();
        }
        if (BufferIsValid(lnode_buffer.buf)) {
            UnlockReleaseBuffer(lnode_buffer.buf);
        }
    }

    if (xl_rec->target != xl_rec->leafblkno) {
        RedoBufferInfo leafnode_buffer;
        if (XLogReadBufferForRedo(record, 3, &leafnode_buffer) == BLK_NEEDS_REDO) {
            DiskNodeImpl leafnode(leafnode_buffer.buf);
            leafnode.get_data(P_HIKEY)->set_ptr(xl_rec->nextchild);
            PageSetLSN(leafnode_buffer.pageinfo.page, lsn);
            leafnode.mark_dirty();
        }
        if (BufferIsValid(leafnode_buffer.buf)) {
            UnlockReleaseBuffer(leafnode_buffer.buf);
        }
    }

    if (BufferIsValid(rnode_buffer.buf)) {
        UnlockReleaseBuffer(rnode_buffer.buf);
    }
    if (BufferIsValid(node_buffer.buf)) {
        UnlockReleaseBuffer(node_buffer.buf);
    }
}

void vectorindex_redo_meta_and_node(XLogReaderState *record)
{
    RedoBufferInfo meta_buf;
    RedoBufferInfo node_buf;

    if (XLogReadBufferForRedo(record, 0, &meta_buf) != BLK_RESTORED) {
        elog(ERROR, "vectorindex record did not contain a full-image of the meta page");
    }

    if (XLogReadBufferForRedo(record, 1, &node_buf) != BLK_RESTORED) {
        elog(ERROR, "vectorindex record did not contain a full-image of the node page");
    }

    if (BufferIsValid(meta_buf.buf)) {
        UnlockReleaseBuffer(meta_buf.buf);
    }
    if (BufferIsValid(node_buf.buf)) {
        UnlockReleaseBuffer(node_buf.buf);
    }
}

void vectorindex_redo_vector_split_scan_blkno(XLogReaderState *record)
{
    XLogRecPtr lsn = record->EndRecPtr;
    RedoBufferInfo buffer;

    if (XLogReadBufferForRedo(record, 0, &buffer) == BLK_NEEDS_REDO) {
        BlockNumber *blkno = (BlockNumber *)XLogRecGetBlockData(record, 0, NULL);
        Page page = buffer.pageinfo.page;
        VectorIndexMeta meta = reinterpret_cast<VectorIndexMeta>(PageGetContents(page));
        meta->split_scanned_blkno = *blkno;
        PageSetLSN(page, lsn);
        MarkBufferDirty(buffer.buf);
    }
    if (BufferIsValid(buffer.buf)) {
        UnlockReleaseBuffer(buffer.buf);
    }
}

void vectorindex_redo_update_next_meta_blkno(XLogReaderState *record)
{
    XLogRecPtr lsn = record->EndRecPtr;
    RedoBufferInfo buffer;

    if (XLogReadBufferForRedo(record, 0, &buffer) == BLK_NEEDS_REDO) {
        BlockNumber *blkno = (BlockNumber *)XLogRecGetBlockData(record, 0, NULL);
        Page page = buffer.pageinfo.page;
        VectorIndexMeta meta = reinterpret_cast<VectorIndexMeta>(PageGetContents(page));
        meta->next_idx_blkno = *blkno;
        PageSetLSN(page, lsn);
        MarkBufferDirty(buffer.buf);
    }
    if (BufferIsValid(buffer.buf)) {
        UnlockReleaseBuffer(buffer.buf);
    }
}

void vectorindex_redo_set_leafmost_node_blkno(XLogReaderState *record)
{
    XLogRecPtr lsn = record->EndRecPtr;
    RedoBufferInfo buffer;

    if (XLogReadBufferForRedo(record, 0, &buffer) == BLK_NEEDS_REDO) {
        BlockNumber *blkno = (BlockNumber *)XLogRecGetBlockData(record, 0, NULL);
        Page page = buffer.pageinfo.page;
        VectorIndexMeta meta = reinterpret_cast<VectorIndexMeta>(PageGetContents(page));
        meta->leftmost_node_blkno = *blkno;
        PageSetLSN(page, lsn);
        MarkBufferDirty(buffer.buf);
    }
    if (BufferIsValid(buffer.buf)) {
        UnlockReleaseBuffer(buffer.buf);
    }
}

void vectorindex_redo_insert_node(XLogReaderState *record)
{
    XLogRecPtr lsn = record->EndRecPtr;
    RedoBufferInfo buffer;

    if (XLogReadBufferForRedo(record, 0, &buffer) == BLK_NEEDS_REDO) {
        Size size;
        BlockNumber *blkno = (BlockNumber *)XLogRecGetBlockData(record, 0, &size);
        Page page = buffer.pageinfo.page;
        VectorIndexNode node = reinterpret_cast<VectorIndexNode>(PageGetContents(page));
        if (size != sizeof(BlockNumber)) {
            node->nodes[nodepage_internal::max_nnode_per_page] = *blkno;
        } else {
            node->insert(InvalidRelation, *blkno, buffer.buf);
        }
        PageSetLSN(page, lsn);
        MarkBufferDirty(buffer.buf);
    }
    if (BufferIsValid(buffer.buf)) {
        UnlockReleaseBuffer(buffer.buf);
    }
}

void vectorindex_redo_remove_node(XLogReaderState *record)
{
    XLogRecPtr lsn = record->EndRecPtr;
    RedoBufferInfo buffer;
    if (XLogReadBufferForRedo(record, 0, &buffer) == BLK_NEEDS_REDO) {
        Page page = buffer.pageinfo.page;
        VectorIndexNode node = reinterpret_cast<VectorIndexNode>(PageGetContents(page));
        BlockNumber *blkno = (BlockNumber *)XLogRecGetBlockData(record, 0, NULL);
        node->remove(InvalidRelation, *blkno);
        PageSetLSN(page, lsn);
        MarkBufferDirty(buffer.buf);
    }
    if (BufferIsValid(buffer.buf)) {
        UnlockReleaseBuffer(buffer.buf);
    }
}

void vectorindex_redo_set_operation(XLogReaderState *record)
{
    XLogRecPtr lsn = record->EndRecPtr;
    RedoBufferInfo buffer;

    if (XLogReadBufferForRedo(record, 0, &buffer) == BLK_NEEDS_REDO) {
        bool *idle = (bool *)XLogRecGetBlockData(record, 0, NULL);
        Page page = buffer.pageinfo.page;
        VectorIndexMeta meta = reinterpret_cast<VectorIndexMeta>(PageGetContents(page));
        meta->idle = *idle;
        PageSetLSN(page, lsn);
        MarkBufferDirty(buffer.buf);
    }
    if (BufferIsValid(buffer.buf)) {
        UnlockReleaseBuffer(buffer.buf);
    }
}

void vectorindex_redo_start_operation(XLogReaderState *record)
{
    vectorindex_redo_set_operation(record);
}

void vectorindex_redo_finish_operation(XLogReaderState *record)
{
    vectorindex_redo_set_operation(record);
}

void vectorindex_redo_set_index_ptr(XLogReaderState *record)
{
    XLogRecPtr lsn = record->EndRecPtr;
    RedoBufferInfo buffer;

    if (XLogReadBufferForRedo(record, 0, &buffer) == BLK_NEEDS_REDO) {
        bool *indexPtrIsNew = (bool *)XLogRecGetData(record);
        char *data = XLogRecGetBlockData(record, 0, NULL);
        size_t *level = (size_t *)data;
        BlockNumber *blkno = (BlockNumber *)(data + sizeof(size_t));
        Page page = buffer.pageinfo.page;
        DiskBTInternalOpaque opaque = reinterpret_cast<DiskBTInternalOpaque>(PageGetSpecialPointer(page));
        if (*indexPtrIsNew) {
            opaque->new_vec_index_meta_blkno[*level] = *blkno;
        } else {
            opaque->vec_index_meta_blkno[*level] = *blkno;
        }
        PageSetLSN(page, lsn);
        MarkBufferDirty(buffer.buf);
    }
    if (BufferIsValid(buffer.buf)) {
        UnlockReleaseBuffer(buffer.buf);
    }
}

void vectorindex_redo_set_deleted(XLogReaderState *record) 
{
    XLogRecPtr lsn = record->EndRecPtr;
    RedoBufferInfo buffer;

    if (XLogReadBufferForRedo(record, 0, &buffer) == BLK_NEEDS_REDO) {
        bool *deleted = (bool *)XLogRecGetBlockData(record, 0, NULL);
        Page page = buffer.pageinfo.page;
        VectorIndexMeta meta = reinterpret_cast<VectorIndexMeta>(PageGetContents(page));
        meta->deleted = *deleted;
        PageSetLSN(page, lsn);
        MarkBufferDirty(buffer.buf);
    }
    if (BufferIsValid(buffer.buf)) {
        UnlockReleaseBuffer(buffer.buf);
    }
}

void vectorindex_redo_set_temp_unlink(XLogReaderState *record)
{
    XLogRecPtr lsn = record->EndRecPtr;
    RedoBufferInfo buffer;
    if (XLogReadBufferForRedo(record, 0, &buffer) == BLK_NEEDS_REDO) {
        bool *isUnlink = (bool *)XLogRecGetBlockData(record, 0, NULL);
        DiskNodeImpl node(buffer.buf);
        node.set_temporarily_unlinkable(*isUnlink);
        PageSetLSN(buffer.pageinfo.page, lsn);
        node.mark_dirty();
    }
    if (BufferIsValid(buffer.buf)) {
        UnlockReleaseBuffer(buffer.buf);
    }
}

void vectorindex_redo_add_ann_meta(XLogReaderState *record)
{
    RedoBufferInfo buffer;

    if (XLogReadBufferForRedo(record, 0, &buffer) != BLK_RESTORED) {
        elog(ERROR, "vectorindex record did not contain a full-image of ann meta page");
    }
    if (BufferIsValid(buffer.buf)) {
        UnlockReleaseBuffer(buffer.buf);
    }
}

void vectorindex_redo_bt_modify_meta(XLogReaderState *record)
{
    XLogRecPtr lsn = record->EndRecPtr;
    RedoBufferInfo meta_buffer;

    VectorIndexType *type = (VectorIndexType *)XLogRecGetData(record);
    if (XLogReadBufferForRedo(record, 0, &meta_buffer) == BLK_NEEDS_REDO) {
        Page page = meta_buffer.pageinfo.page;
        VectorIndexMeta meta = reinterpret_cast<VectorIndexMeta>(PageGetContents(page));
        char *data = XLogRecGetBlockData(record, 0, NULL);
        meta->index_meta_blkno = *(BlockNumber *)data;
        if (*type == VectorIndexType::GRAPH) {
            meta->dim = *(uint32 *)(data + sizeof(BlockNumber));
        }
        PageSetLSN(page, lsn);
        MarkBufferDirty(meta_buffer.buf);
    }
    if (BufferIsValid(meta_buffer.buf)) {
        UnlockReleaseBuffer(meta_buffer.buf);
    }
}

void hybrid_redo(XLogReaderState *record)
{
    uint8 info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;

    switch (info) {
        case XLOG_HYBRID_BUILD_INDEX:
            hybrid_redo_build_index(record);
            break;
        case XLOG_HYBRID_UNLOG_BUILD_INDEX:
            hybrid_redo_unlog_build_index(record);
            break;
        case XLOG_HYBRID_ADD_VECTOR:
            hybrid_redo_write_vec(record);
            break;
        case XLOG_HYBRID_BT_INSERT_DATA:
            hybrid_redo_bt_insert_data(record);
            break;
        case XLOG_HYBRID_BT_SPLIT:
            hybrid_redo_bt_split(record);
            break;
        case XLOG_HYBRID_BT_SPLIT_ROOT:
            hybrid_redo_bt_split_root(record);
            break;
        case XLOG_HYBRID_BT_VACUUM:
            hybrid_redo_bt_vacuum(record);
            break;
        case XLOG_HYBRID_BT_MARK_PAGE_HALFDEAD:
            hybrid_redo_bt_mark_page_halfdead(record);
            break;
        case XLOG_HYBRID_BT_UNLINK_HALFDEAD_PAGE:
            hybrid_redo_bt_unlink_halfdead_page(record);
            break;
        default:
            elog(PANIC, "hybrid_redo: unknown operation code %u", info);
    }
}

void vectorindex_redo(XLogReaderState *record)
{
    uint8 info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;

    switch (info) {
        case XLOG_VECTORINDEX_META_AND_NODE:
            vectorindex_redo_meta_and_node(record);
            break;
        case XLOG_VECTORINDEX_SPLIT_SCAN_BLKNO:
            vectorindex_redo_vector_split_scan_blkno(record);
            break;
        case XLOG_VECTORINDEX_UPDATE_NEXT_META_BLKNO:
            vectorindex_redo_update_next_meta_blkno(record);
            break;
        case XLOG_VECTORINDEX_SET_LEFTMOST_NODE_BLKNO:
            vectorindex_redo_set_leafmost_node_blkno(record);
            break;
        case XLOG_VECTORINDEX_INSERT_NODE:
            vectorindex_redo_insert_node(record);
            break;
        case XLOG_VECTORINDEX_REMOVE_NODE:
            vectorindex_redo_remove_node(record);
            break;
        case XLOG_VECTORINDEX_START_OPERATION:
            vectorindex_redo_start_operation(record);
            break;
        case XLOG_VECTORINDEX_FINISH_OPERATION:
            vectorindex_redo_finish_operation(record);
            break;
        case XLOG_VECTORINDEX_SET_INDEX_PTR:
            vectorindex_redo_set_index_ptr(record);
            break;
        case XLOG_VECTORINDEX_SET_DELETED:
            vectorindex_redo_set_deleted(record);
            break;
        case XLOG_VECTORINDEX_SET_TEMP_UNLINK:
            vectorindex_redo_set_temp_unlink(record);
            break;
        case XLOG_VECTORINDEX_ADD_ANN_META:
            vectorindex_redo_add_ann_meta(record);
            break;
        case XLOG_VECTORINDEX_MODIFY_META:
            vectorindex_redo_bt_modify_meta(record);
            break;
        default:
            elog(PANIC, "vectorindex_redo: unknown operation code %u", info);
    }
}
