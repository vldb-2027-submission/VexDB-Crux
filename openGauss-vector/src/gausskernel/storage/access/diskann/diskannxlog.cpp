/* -------------------------------------------------------------------------
 *
 * diskannxlog.cpp
 *	  WAL replay logic for diskann index.
 *
 * IDENTIFICATION
 *	  src/gausskernel/storage/access/diskann/diskannxlog.cpp
 *
 * -------------------------------------------------------------------------
 */

#include <vtl/disk_container/diskvector.hpp>

#include "postgres.h"
#include "access/diskann/diskann.h"
#include "access/xlogproc.h"
#include "access/xlogreader.h"
#include "storage/buf/bufpage.h"
#include "storage/buf/bufmgr.h"
#include "access/annvector/store/vector_smgr.h"

using namespace disk_container;

static void DiskannRedoSetElem(XLogReaderState *record)
{
    XLogRecPtr lsn = record->EndRecPtr;
    RedoBufferInfo buffer;

    if (XLogReadBufferForRedo(record, 0, &buffer) == BLK_NEEDS_REDO) {
        Page page = buffer.pageinfo.page;

        xl_diskann_add_elem *xl_rec = (xl_diskann_add_elem *)XLogRecGetData(record);
        char *data = XLogRecGetBlockData(record, 0, NULL);
        errno_t rc = memcpy_s((char *)page + xl_rec->offset, xl_rec->elem_size, data, xl_rec->elem_size);
        securec_check(rc, "\0", "\0");

        PageSetLSN(page, lsn);
        MarkBufferDirty(buffer.buf);
    }

    if (BufferIsValid(buffer.buf)) {
        UnlockReleaseBuffer(buffer.buf);
    }
}

void DiskannRedoExtendFullPages(XLogReaderState *record)
{
    uint8 blockId;

    for (blockId = 0; blockId <= XLogRecMaxBlockId(record); blockId++) {
        RedoBufferInfo buffer;
        if (XLogReadBufferForRedo(record, blockId, &buffer) != BLK_RESTORED) {
            elog(ERROR, "unexpected XLogReadBufferForRedo result when restoring backup block");
        }
        if (BufferIsValid(buffer.buf)) {
            UnlockReleaseBuffer(buffer.buf);
        }
    }
}

void DiskannRedoMetapage(XLogReaderState *record)
{
    RedoBufferInfo buffer;
    if (XLogReadBufferForRedo(record, 0, &buffer) != BLK_RESTORED) {
        elog(ERROR, "unexpected XLogReadBufferForRedo result when restoring backup block");
    }
    if (BufferIsValid(buffer.buf)) {
        UnlockReleaseBuffer(buffer.buf);
    }
}

void DiskannRedoUpdateMetaStartNpages(XLogReaderState *record)
{
    XLogRecPtr lsn = record->EndRecPtr;
    RedoBufferInfo buffer;

    if (XLogReadBufferForRedo(record, 0, &buffer) == BLK_NEEDS_REDO) {
        Page page = buffer.pageinfo.page;
        xl_diskann_update_start_npages *xl_rec = (xl_diskann_update_start_npages *)XLogRecGetBlockData(record, 0, NULL);
        DiskVectorMetaPage meta = reinterpret_cast<DiskVectorMetaPage>(PageGetContents(page));
        meta->npage = xl_rec->num_pages;
        meta->item_start_pages[xl_rec->num_pages - 1] = xl_rec->start_blkno;
        PageSetLSN(page, lsn);
        MarkBufferDirty(buffer.buf);
    }

    if (BufferIsValid(buffer.buf)) {
        UnlockReleaseBuffer(buffer.buf);
    }
}

void DiskannUpdateMetaNitem(XLogReaderState *record)
{
    XLogRecPtr lsn = record->EndRecPtr;
    RedoBufferInfo buffer;

    if (XLogReadBufferForRedo(record, 0, &buffer) == BLK_NEEDS_REDO) {
        Page page = buffer.pageinfo.page;
        size_t *nitem = (size_t *)XLogRecGetBlockData(record, 0, NULL);
        DiskVectorMetaPage meta = reinterpret_cast<DiskVectorMetaPage>(PageGetContents(page));
        meta->nitem = *nitem;
        PageSetLSN(page, lsn);
        MarkBufferDirty(buffer.buf);
    }

    if (BufferIsValid(buffer.buf)) {
        UnlockReleaseBuffer(buffer.buf);
    }
}

void DiskannAddVector(XLogReaderState *record)
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

void DiskannInplaceFilterAddData(XLogReaderState *record) 
{
    XLogRecPtr lsn = record->EndRecPtr;
    RedoBufferInfo buffer;

    if (XLogReadBufferForRedo(record, 0, &buffer) == BLK_NEEDS_REDO) {
        Page page = buffer.pageinfo.page;

        xl_diskann_inplace_filter_add_data *xl_rec = (xl_diskann_inplace_filter_add_data *)XLogRecGetData(record);
        char *data = XLogRecGetBlockData(record, 0, NULL);
        errno_t rc = memcpy_s((char *)page + xl_rec->offset, xl_rec->data_size, data, xl_rec->data_size);
        securec_check(rc, "\0", "\0");
        PageSetLSN(page, lsn);
        MarkBufferDirty(buffer.buf);
    }

    if (BufferIsValid(buffer.buf)) {
        UnlockReleaseBuffer(buffer.buf);
    }
}

void DiskannInplaceFilterAddItem(XLogReaderState *record)
{
    XLogRecPtr lsn = record->EndRecPtr;
    RedoBufferInfo buffer;

    if (XLogReadBufferForRedo(record, 0, &buffer) == BLK_NEEDS_REDO) {
        Page page = buffer.pageinfo.page;
        xl_diskann_inplace_filter_add_item *xl_rec = (xl_diskann_inplace_filter_add_item *)XLogRecGetData(record);
        char *data = XLogRecGetBlockData(record, 0, NULL);
        if (xl_rec->isOverWrite) {
            if(!page_index_tuple_overwrite(page, xl_rec->offset, (Item)data, xl_rec->size)) {
                ereport(PANIC, (errmsg("diskann inplace filter add item redo : failed to overwrite item")));
            }
        } else {
            if (PageAddItem(page, (Item)data, xl_rec->size, xl_rec->offset, false, false) == InvalidOffsetNumber) {
                ereport(PANIC, (errmsg("diskann inplace filter add item redo : failed to add item")));
            }
        }
        PageSetLSN(page, lsn);
        MarkBufferDirty(buffer.buf);
    }

    if (BufferIsValid(buffer.buf)) {
        UnlockReleaseBuffer(buffer.buf);
    }
}

void DiskannInplaceFilterDeleteItem(XLogReaderState *record)
{
    XLogRecPtr lsn = record->EndRecPtr;
    RedoBufferInfo buffer;

    if (XLogReadBufferForRedo(record, 0, &buffer) == BLK_NEEDS_REDO) {
        Page page = buffer.pageinfo.page;
        OffsetNumber *offset = (OffsetNumber *)XLogRecGetBlockData(record, 0, NULL);
        PageIndexTupleDelete(page, *offset);
        PageSetLSN(page, lsn);
        MarkBufferDirty(buffer.buf);
    }

    if (BufferIsValid(buffer.buf)) {
        UnlockReleaseBuffer(buffer.buf);
    }
}

void DiskannInplaceFilterMultiDelete(XLogReaderState *record)
{
    XLogRecPtr lsn = record->EndRecPtr;
    RedoBufferInfo buffer;

    if (XLogReadBufferForRedo(record, 0, &buffer) == BLK_NEEDS_REDO) {
        Page page = buffer.pageinfo.page;
        size_t *n = (size_t *)XLogRecGetData(record);
        OffsetNumber *offsets = (OffsetNumber *)XLogRecGetBlockData(record, 0, NULL);
        PageIndexMultiDelete(page, offsets, *n);
        PageSetLSN(page, lsn);
        MarkBufferDirty(buffer.buf);
    }

    if (BufferIsValid(buffer.buf)) {
        UnlockReleaseBuffer(buffer.buf);
    }
}

void DiskannRedoInvalidateVectorCache(XLogReaderState *record)
{
    xl_invalidate_vector_cache *xl_rec = (xl_invalidate_vector_cache *)XLogRecGetData(record);
    vec_invalidate_buffer_cache(xl_rec->relNode, xl_rec->loc, xl_rec->elem_size);
}

void diskann_redo(XLogReaderState *record)
{
    uint8 info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;

    switch (info) {
        case XLOG_DISKANN_BUILD_INDEX:
        case XLOG_DISKANN_UNLOG_BUILD_INDEX:
        case XLOG_DISKANN_EXTEND_NEWPAGES:
            DiskannRedoExtendFullPages(record);
            break;
        case XLOG_DISKANN_SET_ELEM:
            DiskannRedoSetElem(record);
            break;
        case XLOG_DISKANN_UPDATE_META_START_NPAGES:
            DiskannRedoUpdateMetaStartNpages(record);
            break;
        case XLOG_DISKANN_UPDATE_META_NITEM:
            DiskannUpdateMetaNitem(record);
            break;
        case XLOG_DISKANN_ADD_VECTOR:
            DiskannAddVector(record);
            break;
        case XLOG_DISKANN_INPLACE_FILTER_ADD_DATA:
            DiskannInplaceFilterAddData(record);
            break;
        case XLOG_DISKANN_INPLACE_FILTER_ADD_ITEM:
            DiskannInplaceFilterAddItem(record);
            break;
        case XLOG_DISKANN_INPLACE_FILTER_DELETE_ITEM:
            DiskannInplaceFilterDeleteItem(record);
            break;
        case XLOG_DISKANN_INPLACE_FILTER_MULTI_DELETE:
            DiskannInplaceFilterMultiDelete(record);
            break;
        case XLOG_DISKANN_INVALIDATE_VECTOR_CACHE:
            DiskannRedoInvalidateVectorCache(record);
            break;
        default:
            elog(PANIC, "diskann_redo: unknown operation code %u", info);
    }
}
