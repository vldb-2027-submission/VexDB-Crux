#include "postgres.h"
#include "knl/knl_variable.h"
#include "access/annvector/ivf.h"

const char* ivf_type_name(uint8 subtype)
{
    uint8 info = subtype & ~XLR_INFO_MASK;
    switch (info) {
        case XLOG_IVF_UNLOG_BUILD_INDEX:
            return "ivf_unlogged_build_index";
            break;
        case XLOG_IVF_BUILD_INDEX:
            return "ivf_build_index";
            break;
        case XLOG_IVF_EXTEND_NEWPAGES:
            return "ivf_extend_newpages";
            break;
        case XLOG_IVF_INSERT_INDEX:
            return "ivf_insert_index";
            break;
        case XLOG_IVF_DELETE_INDEX:
            return "ivf_delete_index";
            break;
        case XLOG_IVF_UPDATE_LIST:
            return "ivf_update_list";
            break;
        case XLOG_IVF_UPDATE_PAGE_NEXTBLKNO:
            return "ivf_extend_newpage_from_fsm";
            break;
        default:
            return "unknow_type";
            break;
    }
}

void ivf_desc(StringInfo buf, XLogReaderState *record)
{
    uint8 info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;

    switch (info) {
        case XLOG_IVF_UNLOG_BUILD_INDEX:
            appendStringInfoString(buf, "build unlogged index");
            if (XLogRecHasBlockImage(record, 0)) {
                appendStringInfoString(buf, " (full page image)");
            }
            break;
        case XLOG_IVF_BUILD_INDEX:
            appendStringInfoString(buf, "build index");
            if (XLogRecHasBlockImage(record, 0)) {
                appendStringInfoString(buf, " (full page image)");
            }
            break;
        case XLOG_IVF_EXTEND_NEWPAGES:
            appendStringInfoString(buf, "extend new pages");
            if (XLogRecHasBlockImage(record, 0)) {
                appendStringInfoString(buf, " (full page image)");
            }
            break;
        case XLOG_IVF_INSERT_INDEX:
            appendStringInfoString(buf, "insert index");
            break;
        case XLOG_IVF_DELETE_INDEX:
            appendStringInfoString(buf, "delete index");
            break;
        case XLOG_IVF_UPDATE_LIST:
            appendStringInfoString(buf, "update list");
            break;
        case XLOG_IVF_UPDATE_PAGE_NEXTBLKNO:
            appendStringInfoString(buf, "extend newpage from fsm");
            break;
        default:
            appendStringInfo(buf, "unknown ivf op code %hhu", info);
            break;
    }
}