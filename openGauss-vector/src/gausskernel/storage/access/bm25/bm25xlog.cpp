/* -------------------------------------------------------------------------
 *
 * bm25xlog.cpp
 *	  WAL replay logic for bm25.
 *
 * IDENTIFICATION
 *	  src/gausskernel/storage/access/bm25/bm25xlog.cpp
 *
 * -------------------------------------------------------------------------
 */

#include <vtl/bitvector>
#include <vtl/disk_container/disk_hashtable_dependency.hpp>

#include "postgres.h"
#include "storage/buf/bufpage.h"
#include "storage/buf/bufmgr.h"
#include "access/xlogproc.h"
#include "access/xlogreader.h"
#include "access/bm25/bm25.h"
#include "access/bm25/bm25_inverted_list.h"
#include "access/bm25/bm25_statistics.h"

using namespace bm25;
using namespace disk_container;

void Bm25RedoBuildIndex(XLogReaderState *record)
{
    uint8 blockId;

    for (blockId = 0; blockId <= XLogRecMaxBlockId(record); blockId++) {
        RedoBufferInfo buffer;
        if (XLogReadBufferForRedo(record, blockId, &buffer) != BLK_RESTORED) {
            elog(ERROR, "bm25 record did not contain a full-image of index page");
        }
        if (BufferIsValid(buffer.buf)) {
            UnlockReleaseBuffer(buffer.buf);
        }
    }
}

void Bm25RedoInitPage(XLogReaderState *record)
{
    XLogRecPtr lsn = record->EndRecPtr;
    RedoBufferInfo buffer;
    if (XLogReadBufferForRedo(record, 0, &buffer) == BLK_NEEDS_REDO) {
        char *data = XLogRecGetBlockData(record, 0, NULL);
        Page page = buffer.pageinfo.page;
        errno_t rc = memcpy_s(page, BLCKSZ, data, BLCKSZ);
        securec_check(rc, "\0", "\0");
        PageSetLSN(page, lsn);
        MarkBufferDirty(buffer.buf);
    }

    if (BufferIsValid(buffer.buf)) {
        UnlockReleaseBuffer(buffer.buf);
    }
}

void Bm25RedoInsertEntry(XLogReaderState *record)
{
    XLogRecPtr lsn = record->EndRecPtr;
    RedoBufferInfo buffer;
    if (XLogReadBufferForRedo(record, 0, &buffer) == BLK_NEEDS_REDO) {
        Page page = buffer.pageinfo.page;
        xl_bm25_insert_entry *xl_rec = (xl_bm25_insert_entry *)XLogRecGetData(record);
        char *data = XLogRecGetBlockData(record, 0, NULL);

        errno_t rc = memcpy_s((char *)page + xl_rec->offset, xl_rec->entry_size, data, xl_rec->entry_size);
        securec_check(rc, "\0", "\0");

        InvertedListPage il_page = GetInvertedListPage(page);
        il_page->nentry = xl_rec->nentry;
        PageSetLSN(page, lsn);
        MarkBufferDirty(buffer.buf);
    }

    if (BufferIsValid(buffer.buf)) {
        UnlockReleaseBuffer(buffer.buf);
    }
}

void Bm25RedoInsertInplaceEntry(XLogReaderState *record)
{
    XLogRecPtr lsn = record->EndRecPtr;
    RedoBufferInfo buffer;

    if (XLogReadBufferForRedo(record, 0, &buffer) == BLK_NEEDS_REDO) {
        Page page = buffer.pageinfo.page;
        xl_bm25_insert_inplace_entry *xl_rec = (xl_bm25_insert_inplace_entry *)XLogRecGetData(record);
        char *data = XLogRecGetBlockData(record, 0, NULL);

        errno_t rc = memcpy_s((char *)page + xl_rec->bit_offset, xl_rec->bit_size, data, xl_rec->bit_size);
        securec_check(rc, "\0", "\0");
        rc = memcpy_s((char *)page + xl_rec->offset, xl_rec->entry_size, (char *)(data + xl_rec->bit_size), xl_rec->entry_size);
        securec_check(rc, "\0", "\0");

        uint16 *nentry = (uint16 *)(page + MAXALIGN(SizeOfPageHeaderData) + sizeof(uint32) + sizeof(uint16));
        *nentry = xl_rec->nentry;

        PageSetLSN(buffer.pageinfo.page, lsn);
        MarkBufferDirty(buffer.buf);
    }
    if (BufferIsValid(buffer.buf)) {
        UnlockReleaseBuffer(buffer.buf);
    }
}

void Bm25RedoInsertStats(XLogReaderState *record)
{
    XLogRecPtr lsn = record->EndRecPtr;
    RedoBufferInfo buffer;

    if (XLogReadBufferForRedo(record, 0, &buffer) == BLK_NEEDS_REDO) {
        Page page = buffer.pageinfo.page;
        xl_bm25_insert_stats *xl_rec = (xl_bm25_insert_stats *)XLogRecGetData(record);
        if (xl_rec->data_size > 0) {
            char *data = XLogRecGetBlockData(record, 0 ,NULL);
            errno_t rc = memcpy_s((char *)page + xl_rec->offset, xl_rec->data_size,
                                  data, xl_rec->data_size);
            securec_check(rc, "\0", "\0");
        }

        BM25StatisticsPage bm_page = ((BM25StatisticsPage)((char *)(page) + MAXALIGN(SizeOfPageHeaderData)));
        bm_page->max_doc_id = xl_rec->max_doc_id;

        PageSetLSN(page, lsn);
        MarkBufferDirty(buffer.buf);
    }

    if (BufferIsValid(buffer.buf)) {
        UnlockReleaseBuffer(buffer.buf);
    }
}

void Bm25RedoUpdateInvertList(XLogReaderState *record)
{
    XLogRecPtr lsn = record->EndRecPtr;
    RedoBufferInfo buffer;

    if (XLogReadBufferForRedo(record, 0, &buffer) == BLK_NEEDS_REDO) {
        Page page = buffer.pageinfo.page;
        xl_bm25_update_invert_list *xl_rec = (xl_bm25_update_invert_list *)XLogRecGetData(record);
        char *data = XLogRecGetBlockData(record, 0, NULL);

        errno_t rc = memcpy_s((char *)page + xl_rec->offset, xl_rec->entry_size, data, xl_rec->entry_size);
        securec_check(rc, "\0", "\0");

        InvertedListPage il_page = GetInvertedListPage(page);
        il_page->nentry = xl_rec->nentry;

        PageSetLSN(page, lsn);
        MarkBufferDirty(buffer.buf);
    }

    if (BufferIsValid(buffer.buf)) {
        UnlockReleaseBuffer(buffer.buf);
    }
}

void Bm25RedoAddData(XLogReaderState *record)
{
    XLogRecPtr lsn = record->EndRecPtr;
    RedoBufferInfo buffer;

    if (XLogReadBufferForRedo(record, 0, &buffer) == BLK_NEEDS_REDO) {
        Page page = buffer.pageinfo.page;

        xl_bm25_add_data *xl_rec = (xl_bm25_add_data *)XLogRecGetData(record);
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

void bm25_redo(XLogReaderState *record)
{
    uint8 info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;
    switch (info) {
        case XLOG_BM25_BUILD_INDEX:
        case XLOG_BM25_APPEND_PAGES:
            Bm25RedoBuildIndex(record);
            break;
        case XLOG_BM25_INIT_PAGE:
            Bm25RedoInitPage(record);
            break;
        case XLOG_BM25_INSERT_ENTRY:
            Bm25RedoInsertEntry(record);
            break;
        case XLOG_BM25_INSERT_INPLACE_ENTRY:
            Bm25RedoInsertInplaceEntry(record);
            break;
        case XLOG_BM25_INSERT_STATS:
            Bm25RedoInsertStats(record);
            break;
        case XLOG_BM25_UPDATE_INVERT_LIST:
            Bm25RedoUpdateInvertList(record);
            break;
        case XLOG_BM25_ADD_DATA:
            Bm25RedoAddData(record);
            break;
        default:
            elog(PANIC, "bm25_redo: unknown op code %u", info);
    }
}

void Bm25XLogInitPage(Buffer buffer, Page page)
{
    XLogBeginInsert();
    START_CRIT_SECTION();
    XLogRegisterBuffer(0, buffer, REGBUF_STANDARD);
    XLogRegisterBufData(0, page, BLCKSZ);
    XLogRecPtr recptr = XLogInsert(RM_BM25_ID, XLOG_BM25_INIT_PAGE);
    PageSetLSN(page, recptr);
    END_CRIT_SECTION();
}

void Bm25XLogAppendPage(Buffer buffer, Page page)
{
    XLogBeginInsert();
    START_CRIT_SECTION();
    XLogRegisterBuffer(0, buffer, REGBUF_FORCE_IMAGE | REGBUF_STANDARD);
    XLogRecPtr recptr = XLogInsert(RM_BM25_ID, XLOG_BM25_APPEND_PAGES);
    PageSetLSN(page, recptr);
    END_CRIT_SECTION();
}

void Bm25XLogInsertEntry(Buffer buffer, Page page, const xl_bm25_insert_entry &xl_rec)
{
    XLogBeginInsert();
    START_CRIT_SECTION();
    XLogRegisterData((char *)&xl_rec, sizeof(xl_bm25_insert_entry));
    XLogRegisterBuffer(0, buffer, REGBUF_STANDARD);
    XLogRegisterBufData(0, (char *)(page + xl_rec.offset), xl_rec.entry_size);
    XLogRecPtr recptr = XLogInsert(RM_BM25_ID, XLOG_BM25_INSERT_ENTRY);
    PageSetLSN(page, recptr);
    END_CRIT_SECTION();
}

void Bm25XLogInsertInplaceEntry(Buffer buffer, Page page, const xl_bm25_insert_inplace_entry &xl_rec)
{
    XLogBeginInsert();
    START_CRIT_SECTION();
    XLogRegisterData((char *)&xl_rec, sizeof(xl_bm25_insert_inplace_entry));
    XLogRegisterBuffer(0, buffer, REGBUF_STANDARD);
    XLogRegisterBufData(0, (char *)(page + xl_rec.bit_offset), xl_rec.bit_size);
    XLogRegisterBufData(0, (char *)(page + xl_rec.offset), xl_rec.entry_size);
    XLogRecPtr recptr = XLogInsert(RM_BM25_ID, XLOG_BM25_INSERT_INPLACE_ENTRY);
    PageSetLSN(page, recptr);
    END_CRIT_SECTION();
}

void Bm25XLogInsertStats(Buffer buffer, Page page, const xl_bm25_insert_stats &xl_rec)
{
    XLogBeginInsert();
    START_CRIT_SECTION();
    XLogRegisterData((char *)&xl_rec, sizeof(xl_bm25_insert_stats));
    XLogRegisterBuffer(0, buffer, REGBUF_STANDARD);
    if (xl_rec.data_size > 0) {
        XLogRegisterBufData(0, (char *)(page + xl_rec.offset), xl_rec.data_size);
    }
    XLogRecPtr recptr = XLogInsert(RM_BM25_ID, XLOG_BM25_INSERT_STATS);
    PageSetLSN(page, recptr);
    END_CRIT_SECTION();
}
void Bm25XLogAddData(Buffer buffer, Page page, const xl_bm25_add_data &xl_rec)
{
    XLogBeginInsert();
    START_CRIT_SECTION();
    XLogRegisterData((char *)&xl_rec, sizeof(xl_bm25_add_data));
    XLogRegisterBuffer(0, buffer, REGBUF_STANDARD);
    XLogRegisterBufData(0, (char *)(page + xl_rec.offset), xl_rec.data_size);
    XLogRecPtr recptr = XLogInsert(RM_BM25_ID, XLOG_BM25_ADD_DATA);
    PageSetLSN(page, recptr);
    END_CRIT_SECTION();
}

void Bm25XLogUpdateInvertList(Buffer buffer, Page page, const xl_bm25_update_invert_list &xl_rec)
{
    XLogBeginInsert();
    START_CRIT_SECTION();
    XLogRegisterData((char *)&xl_rec, sizeof(xl_bm25_update_invert_list));
    XLogRegisterBuffer(0, buffer, REGBUF_STANDARD);
    XLogRegisterBufData(0, (char *)(page + xl_rec.offset), xl_rec.entry_size);
    XLogRecPtr recptr = XLogInsert(RM_BM25_ID, XLOG_BM25_UPDATE_INVERT_LIST);
    PageSetLSN(page, recptr);
    END_CRIT_SECTION();
}