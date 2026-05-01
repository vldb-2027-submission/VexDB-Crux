/**
 * Copyright ...
 */

#ifndef LOG_MANAGER_H
#define LOG_MANAGER_H

#include <vtl/disk_container/blockmgr.hpp>
#include "utils/relcache.h"
#include "access/xlogproc.h"

class LogManager {
    using PageData = disk_container::PageData;
public:
    Relation _rel;
    explicit LogManager(Relation rel) : _rel(rel) {}
    void log_build_index(ForkNumber fork_number, bool use_btree);
    void diskann_xlog_add_elem(PageData &page_data, char *elem_data, size_t elem_size, uint32 offset);
    void diskann_extend_newpages(BlockNumber start_blkno, BlockNumber end_blkno);
    void diskann_update_meta_start_npages(PageData &meta_buf, uint32 npage, BlockNumber start_blkno);
    void diskann_update_meta_nitem(PageData &meta_buf, size_t nitem);
    void diskann_inplace_filter_add_data(Buffer buf, Page page, uint32 offset, size_t data_size);
    void diskann_inplace_filter_add_item(Buffer buf, Page page, char *item, size_t size, OffsetNumber offset, bool isOverWrite);
    void diskann_inplace_filter_delete_item(Buffer buf, Page page, OffsetNumber offset);
    void diskann_inplace_filter_multi_delete(Buffer buf, Page page, OffsetNumber *offsets, size_t n);
    void log_write_vector(off_t offset, int nbytes, char *vec, bool use_btree);
    void log_build_vector(Relation heap, Relation index, uint32 dim, size_t size, bool use_btree);
    void log_vecindex_set_temp_unlink(Buffer buffer, Page page, bool isUnlink);
    void log_vecindex_set_operation(Buffer buffer, bool idle);
    void log_vecindex_meta_and_node(BlockNumber new_meta_blkno);
    void log_vecindex_set_index_ptr(Buffer buffer, Page page, size_t level, BlockNumber blkno,
                                    bool indexPtrIsNew);
    void log_invalidate_vector_cache(Relation rel, size_t loc, size_t elem_size, RmgrId rmid, uint8 info);
    void destroy() {}
};

#define XLOG_HYBRID_BUILD_INDEX 0x00
#define XLOG_HYBRID_UNLOG_BUILD_INDEX 0x10
#define XLOG_HYBRID_ADD_VECTOR 0x20
#define XLOG_HYBRID_BT_INSERT_DATA 0x30
#define XLOG_HYBRID_BT_SPLIT   0x40
#define XLOG_HYBRID_BT_SPLIT_ROOT 0x50
#define XLOG_HYBRID_BT_VACUUM 0x60
#define XLOG_HYBRID_BT_MARK_PAGE_HALFDEAD 0x70
#define XLOG_HYBRID_BT_UNLINK_HALFDEAD_PAGE 0x80

#define XLOG_VECTORINDEX_META_AND_NODE 0x00
#define XLOG_VECTORINDEX_SPLIT_SCAN_BLKNO 0x10
#define XLOG_VECTORINDEX_UPDATE_NEXT_META_BLKNO 0x20
#define XLOG_VECTORINDEX_SET_LEFTMOST_NODE_BLKNO 0x30
#define XLOG_VECTORINDEX_INSERT_NODE 0x40
#define XLOG_VECTORINDEX_REMOVE_NODE 0x50
#define XLOG_VECTORINDEX_START_OPERATION 0x60
#define XLOG_VECTORINDEX_FINISH_OPERATION 0x70
#define XLOG_VECTORINDEX_SET_INDEX_PTR 0x80
#define XLOG_VECTORINDEX_SET_DELETED 0x90
#define XLOG_VECTORINDEX_SET_TEMP_UNLINK 0xA0
#define XLOG_VECTORINDEX_ADD_ANN_META 0xB0
#define XLOG_VECTORINDEX_MODIFY_META 0xC0

extern void hybrid_redo(XLogReaderState *record);
extern void hybrid_desc(StringInfo buf, XLogReaderState *record);
extern const char* hybrid_type_name(uint8 subtype);

extern void vectorindex_redo(XLogReaderState *record);
extern void vectorindex_desc(StringInfo buf, XLogReaderState *record);
extern const char* vectorindex_type_name(uint8 subtype);

typedef struct xl_hybrid_split {
    bool is_right_most;
    bool isleaf;
} xl_hybrid_split;

typedef struct xl_hybrid_mark_page_harfhead {
    OffsetNumber topoff;
    BlockNumber rightsib;
} xl_hybrid_mark_page_harfhead;

typedef struct xl_hybrid_unlink_harfhead_page {
    BlockNumber leftsib;
    BlockNumber rightsib;
    BlockNumber nextchild;
    BlockNumber target;
    BlockNumber leafblkno;
    TransactionId xid;
} xl_hybrid_unlink_harfhead_page;

typedef struct xl_diskann_add_elem {
    uint32 offset;
    size_t elem_size;
} xl_diskann_add_elem;

typedef struct xl_diskann_update_start_npages {
    uint32 num_pages;
    BlockNumber start_blkno;
} xl_diskann_update_start_npages;

typedef struct xl_ann_add_vector {
    RelFileNode r_Node;
    off_t offset;
    int nbytes;
} xl_ann_add_vector;

typedef struct xl_diskann_inplace_filter_add_data {
    uint32 offset;
    size_t data_size;
} xl_diskann_inplace_filter_add_data;

typedef struct xl_diskann_inplace_filter_add_item {
    size_t size;
    OffsetNumber offset;
    bool isOverWrite;
} xl_diskann_inplace_filter_add_item;

typedef struct xl_invalidate_vector_cache {
    RelFileNode r_Node;
    Oid relNode;
    size_t loc;
    size_t elem_size;
} xl_invalidate_vector_cache;

#endif /* LOG_MANAGER_H */
