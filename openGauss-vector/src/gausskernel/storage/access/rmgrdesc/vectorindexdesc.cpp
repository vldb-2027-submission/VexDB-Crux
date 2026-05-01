#include "postgres.h"
#include "knl/knl_variable.h"
#include "access/annvector/xlog/log_manager.h"

const char* vectorindex_type_name(uint8 subtype)
{
    uint8 info = subtype & ~XLR_INFO_MASK;
    switch (info) {
        case XLOG_VECTORINDEX_META_AND_NODE:
            return "vectorindex_prepare_meta";
            break;
        case XLOG_VECTORINDEX_SPLIT_SCAN_BLKNO:
            return "vectorindex_split_scan_blkno";
            break;
        case XLOG_VECTORINDEX_UPDATE_NEXT_META_BLKNO:
            return "vectorindex_update_next_meta_blkno";
            break;
        case XLOG_VECTORINDEX_SET_LEFTMOST_NODE_BLKNO:
            return "vectorindex_set_leftmost_node_blkno";
            break;
        case XLOG_VECTORINDEX_INSERT_NODE:
            return "vectorindex_insert_node";
            break;
        case XLOG_VECTORINDEX_REMOVE_NODE:
            return "vectorindex_remove_node";
            break;
        case XLOG_VECTORINDEX_START_OPERATION:
            return "vectorindex_start_operation";
            break;
        case XLOG_VECTORINDEX_FINISH_OPERATION:
            return "vectorindex_finish_opration";
            break;
        case XLOG_VECTORINDEX_SET_INDEX_PTR:
            return "vectorIndex_set_index_ptr";
            break;
        case XLOG_VECTORINDEX_SET_DELETED:
            return "vectorIndex_set_deleted";
            break;
        case XLOG_VECTORINDEX_SET_TEMP_UNLINK:
            return "vectorIndex_set_temp_unlink";
            break;
        case XLOG_VECTORINDEX_ADD_ANN_META:
            return "vectorindex_add_ann_meta";
            break;
        case XLOG_VECTORINDEX_MODIFY_META:
            return "vectorIndex_modify_meta";
            break;
        default:
            return "unknow_type";
            break;
    }
}

void vectorindex_desc(StringInfo buf, XLogReaderState *record)
{
    uint8 info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;

    switch (info) {
        case XLOG_VECTORINDEX_META_AND_NODE:
            appendStringInfoString(buf, "vectorindex prepare meta");
            break;
        case XLOG_VECTORINDEX_SPLIT_SCAN_BLKNO:
            appendStringInfoString(buf, "vectorindex split scan blkno");
            break;
        case XLOG_VECTORINDEX_UPDATE_NEXT_META_BLKNO:
            appendStringInfoString(buf, "vectorindex update next meta blkno");
            break;
        case XLOG_VECTORINDEX_SET_LEFTMOST_NODE_BLKNO:
            appendStringInfoString(buf, "vectorindex set leftmost node blkno");
            break;
        case XLOG_VECTORINDEX_INSERT_NODE:
            appendStringInfoString(buf, "vectorindex insert node");
            break;
        case XLOG_VECTORINDEX_REMOVE_NODE:
            appendStringInfoString(buf, "vectorindex remove node");
            break;
        case XLOG_VECTORINDEX_START_OPERATION:
            appendStringInfoString(buf, "vectorindex start operation");
            break;
        case XLOG_VECTORINDEX_FINISH_OPERATION:
            appendStringInfoString(buf, "vectorindex finish opration");
            break;
        case XLOG_VECTORINDEX_SET_INDEX_PTR:
            appendStringInfoString(buf, "vectorindex set index ptr");
            break;
        case XLOG_VECTORINDEX_SET_DELETED:
            appendStringInfoString(buf, "vectorindex set deleted");
            break;
        case XLOG_VECTORINDEX_SET_TEMP_UNLINK:
            appendStringInfoString(buf, "vectorindex set temp unlink");
            break;
        case XLOG_VECTORINDEX_ADD_ANN_META:
            appendStringInfoString(buf, "vectorindex add ann meta");
            break;
        case XLOG_VECTORINDEX_MODIFY_META:
            appendStringInfoString(buf, "vectorindex modify meta");
            break;
        default:
            appendStringInfo(buf, "unknown vectorindex op code %hhu", info);
            break;
    }
}
