#include "postgres.h"
#include "knl/knl_variable.h"
#include "access/hnsw/hnsw_xlog.h"

const char* hnsw_type_name(uint8 subtype)
{
    uint8 info = subtype & ~XLR_INFO_MASK;
    switch (info) {
        case XLOG_HNSW_UNLOG_BUILD_INDEX:
            return "hnsw_unlog_build_index";
        case XLOG_HNSW_BUILD_INDEX:
            return "hnsw_build_index";
        case XLOG_HNSW_ADD_HEAP_TID:
            return "hnsw_add_heap_tid";
        case XLOG_HNSW_ADD_ELEMENT:
            return "hnsw_add_element";
        case XLOG_HNSW_META:
            return "hnsw_update_meta";
        case XLOG_HNSW_UPDATE_NEIGHBORPAGE:
            return "hnsw_update_neighborpage";
        case XLOG_HNSW_APPEND_PAGE:
            return "hnsw_append_page";
        case XLOG_HNSW_UPDATE_NEXTBLKNO:
            return "hnsw_update_nextblkno";
        case XLOG_HNSW_REMOVE_HEAP_TID:
            return "hnsw_remove_heap_tid";
        case XLOG_HNSW_REPAIR_GRAPH_ELEMENT:
            return "hnsw_repair_graph_element";
        case XLOG_HNSW_MARK_DELETE:
            return "hnsw_mark_delete";
        case XLOG_HNSW_ADD_VECTOR:
            return "hnsw_add_vector";
        case XLOG_HNSW_INVALIDATE_VECTOR_CACHE:
            return "hnsw_invalidate_vector_cache";
        case XLOG_HNSW_WRITE_NEIGHBOR:
            return "hnsw_write_neighbor";
        case XLOG_HNSW_TUPLE:
            return "hnsw_tuple";
            break;
        case XLOG_HNSW_UPDATE_QT:
            return "hnsw_update_quantizer";
            break;
        default:
            return "unknow_type";
    }
}

void hnsw_desc(StringInfo buf, XLogReaderState *record)
{
    uint8 info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;
    switch (info) {
        case XLOG_HNSW_UNLOG_BUILD_INDEX:
            appendStringInfoString(buf, "build unlogged hnsw index");
            if (XLogRecHasBlockImage(record, 0)) {
                appendStringInfoString(buf, " (full page image)");
            }
            break;
        case XLOG_HNSW_BUILD_INDEX:
            appendStringInfoString(buf, "build hnsw index");
            if (XLogRecHasBlockImage(record, 0)) {
                appendStringInfoString(buf, " (full page image)");
            }
            break;
        case XLOG_HNSW_ADD_HEAP_TID:
            appendStringInfoString(buf, "hnsw add heap_tid");
            break;
        case XLOG_HNSW_ADD_ELEMENT:
            appendStringInfoString(buf, "hnsw add element");
            break;
        case XLOG_HNSW_META:
            appendStringInfoString(buf, "hnsw update meta");
            break;
        case XLOG_HNSW_UPDATE_NEIGHBORPAGE:
            appendStringInfoString(buf, "hnsw update metadata");
            break;
        case XLOG_HNSW_APPEND_PAGE:
            appendStringInfoString(buf, "hnsw append page");
            if (XLogRecHasBlockImage(record, 0)) {
                appendStringInfoString(buf, " (full page image)");
            }
            break;
        case XLOG_HNSW_UPDATE_NEXTBLKNO:
            appendStringInfoString(buf, "hnsw update nextblkno");
            break;
        case XLOG_HNSW_REMOVE_HEAP_TID:
            appendStringInfoString(buf, "hnsw remove heap tid");
            break;
        case XLOG_HNSW_REPAIR_GRAPH_ELEMENT:
            appendStringInfoString(buf, "hnsw repair graph element");
            break;
        case XLOG_HNSW_MARK_DELETE:
            appendStringInfoString(buf, "hnsw mark delete");
            break;
        case XLOG_HNSW_ADD_VECTOR:
            appendStringInfoString(buf, "hnsw add vector");
            break;
        case XLOG_HNSW_INVALIDATE_VECTOR_CACHE:
            appendStringInfoString(buf, "hnsw invalidate vector cache");
            break;
        case XLOG_HNSW_WRITE_NEIGHBOR:
            appendStringInfoString(buf, "hnsw write neighbor");
            break;
        case XLOG_HNSW_TUPLE:
            appendStringInfoString(buf, "hnsw write tuple");
        case XLOG_HNSW_UPDATE_QT:
            appendStringInfoString(buf, "hnsw update quantizer");
            break;
        default:
            appendStringInfo(buf, "unknown hnsw op code %hhu", info);
            break;
    }
}
