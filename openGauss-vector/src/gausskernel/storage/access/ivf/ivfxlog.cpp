/* -------------------------------------------------------------------------
 *
 * ivfxlog.cpp
 *	  WAL replay logic for ivf index.
 *
 * IDENTIFICATION
 *	  src/gausskernel/storage/access/nbtree/ivfxlog.cpp
 *
 * -------------------------------------------------------------------------
 */

#include "postgres.h"
#include "access/annvector/ivf.h"
#include "access/xlogproc.h"
#include "access/xlogreader.h"
#include "storage/buf/bufpage.h"
#include "storage/buf/bufmgr.h"

static void IvfRedoBuildIndex(XLogReaderState *record)
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

static void IvfRedoExtendNewPage(XLogReaderState *record)
{
    IvfRedoBuildIndex(record);
}

static void IvfRedoBuildUnlogIndex(XLogReaderState *record)
{
    IvfRedoBuildIndex(record);
}

static void IvfRedoInsertIndex(XLogReaderState *record)
{
    RedoBufferInfo buffer;
    XLogRecPtr lsn = record->EndRecPtr;

    if (XLogReadBufferForRedo(record, 0, &buffer) == BLK_NEEDS_REDO) {
        Page page = buffer.pageinfo.page;
        xl_ivf_insert *xl_rec = (xl_ivf_insert *)XLogRecGetData(record);
        char *itup = XLogRecGetBlockData(record, 0, NULL);

        if (OffsetNumberIsValid(xl_rec->reuseOffno)) {
            if (!page_index_tuple_overwrite(page, xl_rec->reuseOffno, (Item) itup, xl_rec->itemsz)) {
                ereport(PANIC, (errmsg("ivf index insert redo : failed to overwrite itup")));
            }
        } else {
            if (PageAddItem(page, (Item) itup, xl_rec->itemsz, xl_rec->offsetNumber, false, false) == InvalidOffsetNumber) {
                ereport(PANIC, (errmsg("ivf index insert redo : failed to add itup")));
            }
        }
        PageSetLSN(page, lsn);
        MarkBufferDirty(buffer.buf);
    }
    if (BufferIsValid(buffer.buf)) {
        UnlockReleaseBuffer(buffer.buf);
    }
}

static void IvfRedoDeleteIndex(XLogReaderState *record)
{
    RedoBufferInfo buffer;
    XLogRecPtr lsn = record->EndRecPtr;

    if (XLogReadBufferForRedo(record, 0, &buffer) == BLK_NEEDS_REDO) {
        Page page = buffer.pageinfo.page;
        xl_ivf_vacuum *xl_rec = (xl_ivf_vacuum *)XLogRecGetData(record);
        OffsetNumber *deletable = (OffsetNumber *)XLogRecGetBlockData(record, 0, NULL);
        if (xl_rec->flag == DELETE_PHYSICAL) {
            PageIndexMultiDelete(page, deletable, xl_rec->ndeletable);
        } else if (xl_rec->flag == DELETE_MARK_UNUSED) {
            PageIndexMultiSetUnused(page, deletable, xl_rec->ndeletable);
        } else {
            elog(ERROR, "unexpected ivf redo delete index method:%u", xl_rec->flag);
        }
        PageSetLSN(page, lsn);
        MarkBufferDirty(buffer.buf);
    }

    if (BufferIsValid(buffer.buf)) {
        UnlockReleaseBuffer(buffer.buf);
    }
}

static void IvfRedoUpdateList(XLogReaderState *record)
{
    RedoBufferInfo	buffer;
    XLogRecPtr lsn = record->EndRecPtr;

    if (XLogReadBufferForRedo(record, 0, &buffer) == BLK_NEEDS_REDO) {
        Page page = buffer.pageinfo.page;
        OffsetNumber *offsetNumber = (OffsetNumber *)XLogRecGetData(record);
        char *data = XLogRecGetBlockData(record, 0, NULL);
        IvfList list = (IvfList) PageGetItem(page, PageGetItemId(page, *offsetNumber));
        errno_t rc = memcpy_s((char*)&list->startPage, 4 * sizeof(BlockNumber), data, 4 * sizeof(BlockNumber));
        securec_check(rc, "\0", "\0");
        PageSetLSN(page, lsn);
        MarkBufferDirty(buffer.buf);
    }
    if (BufferIsValid(buffer.buf)) {
        UnlockReleaseBuffer(buffer.buf);
    }
}

static void IvfRedoUpdatePageNextBlkno(XLogReaderState *record)
{
    XLogRecPtr	lsn = record->EndRecPtr;
    RedoBufferInfo	buffer;

    XLogRedoAction action = XLogReadBufferForRedo(record, 0, &buffer);
    if (action == BLK_NEEDS_REDO)
    {
        size_t datalen;
        char *blockDelta = XLogRecGetBlockData(record, 0, &datalen);
        Page page = buffer.pageinfo.page;
        errno_t rc = memcpy_s(PageGetSpecialPointer(page), datalen, blockDelta, datalen);
        securec_check(rc, "\0", "\0");
        PageSetLSN(page, lsn);
        MarkBufferDirty(buffer.buf);
    }
    if (BufferIsValid(buffer.buf)) {
        UnlockReleaseBuffer(buffer.buf);
    }
}

void ivf_redo(XLogReaderState *record)
{
    uint8 info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;

    switch (info) {
        case XLOG_IVF_UNLOG_BUILD_INDEX:
            IvfRedoBuildUnlogIndex(record);
            break;
        case XLOG_IVF_BUILD_INDEX:
            IvfRedoBuildIndex(record);
            break;
        case XLOG_IVF_EXTEND_NEWPAGES:
            IvfRedoExtendNewPage(record);
            break;
        case XLOG_IVF_INSERT_INDEX:
            IvfRedoInsertIndex(record);
            break;
        case XLOG_IVF_DELETE_INDEX:
            IvfRedoDeleteIndex(record);
            break;
        case XLOG_IVF_UPDATE_LIST:
            IvfRedoUpdateList(record);
            break;
        case XLOG_IVF_UPDATE_PAGE_NEXTBLKNO:
            IvfRedoUpdatePageNextBlkno(record);
            break;
        default:
            elog(PANIC, "ivf_redo: unknown op code %u", info);
    }
}
