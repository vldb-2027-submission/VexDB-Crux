#include "postgres.h"
#include "knl/knl_variable.h"
#include "access/bm25/bm25.h"


const char* bm25_type_name(uint8 subtype)
{
    uint8 info = subtype & ~XLR_INFO_MASK;
    switch (info) {
        case XLOG_BM25_BUILD_INDEX:
            return "bm25_build_index";
            break;
        case XLOG_BM25_INIT_PAGE:
            return "bm25_init_page";
            break;
        case XLOG_BM25_APPEND_PAGES:
            return "bm25_append_pages";
            break;
        case XLOG_BM25_INSERT_ENTRY:
            return "bm25_insert_entry";
            break;
        case XLOG_BM25_INSERT_INPLACE_ENTRY:
            return "bm25_insert_inplace_entry";
            break;
        case XLOG_BM25_INSERT_STATS:
            return "bm25_insert_stats";
            break;
        case XLOG_BM25_UPDATE_INVERT_LIST:
            return "bm25_update_invert_list";
            break;
        case XLOG_BM25_ADD_DATA:
            return "bm25_add_data";
            break;
        default:
            return "unknow_type";
            break;
    }   
}

void bm25_desc(StringInfo buf, XLogReaderState *record)
{
    uint8 info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;

    switch (info) {
        case XLOG_BM25_BUILD_INDEX:
            appendStringInfoString(buf, "bm25 build index");
            if (XLogRecHasBlockImage(record, 0)) {
                appendStringInfoString(buf, " (full page image)");
            }
            break;
        case XLOG_BM25_INIT_PAGE:
            appendStringInfoString(buf, "bm25 init page");
            break;
        case XLOG_BM25_APPEND_PAGES:
            appendStringInfoString(buf, "bm25 append pages");
            break;
        case XLOG_BM25_INSERT_ENTRY:
            appendStringInfoString(buf, "bm25 insert entry");
            break;
        case XLOG_BM25_INSERT_INPLACE_ENTRY:
            appendStringInfoString(buf, "bm25 insert inplace entry");
            break;
        case XLOG_BM25_INSERT_STATS:
            appendStringInfoString(buf, "bm25 insert stats");
            break;
        case XLOG_BM25_UPDATE_INVERT_LIST:
            appendStringInfoString(buf, "bm25 update invert list");
            break;
        case XLOG_BM25_ADD_DATA:
            appendStringInfoString(buf, "bm25 add data");
            break;
        default:
            appendStringInfo(buf, "unknown bm25 op code %hhu", info);
            break;
    }
}