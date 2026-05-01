#ifndef HNSW_XLOG_H
#define HNSW_XLOG_H

#include "postgres.h"
#include "access/hnsw/hnsw_struct.h"

/* WAL record definitions for hnsw's WAL operations */
#define XLOG_HNSW_UNLOG_BUILD_INDEX 0x00
#define XLOG_HNSW_BUILD_INDEX 0x10
#define XLOG_HNSW_ADD_HEAP_TID 0x20
#define XLOG_HNSW_ADD_ELEMENT 0x30
#define XLOG_HNSW_META 0x40
#define XLOG_HNSW_UPDATE_NEIGHBORPAGE 0x50
#define XLOG_HNSW_APPEND_PAGE 0x60
#define XLOG_HNSW_UPDATE_NEXTBLKNO 0x70
#define XLOG_HNSW_REMOVE_HEAP_TID 0x80
#define XLOG_HNSW_REPAIR_GRAPH_ELEMENT 0x90
#define XLOG_HNSW_MARK_DELETE 0xA0
#define XLOG_HNSW_ADD_VECTOR  0xB0
#define XLOG_HNSW_INVALIDATE_VECTOR_CACHE 0xC0
#define XLOG_HNSW_WRITE_NEIGHBOR 0xD0
#define XLOG_HNSW_TUPLE 0xE0
#define XLOG_HNSW_UPDATE_QT 0xF0

enum class HNSW_META_XLOG_TYPE : uint8 {
    UPDATE_NUM_VECTOR,
    UPDATE_INSERT_PAGE,
    UPDATE_ENTRY_POINT,
    UPDATE_ALL = 0xA9u,     /* for compatibility, first 8 bits of HNSW_MAGIC_NUMBER */
    UPDATE_ALL2 = 0x53u,    /* last 8 bits of HNSW_MAGIC_NUMBER, for lettle-endian */
};

enum class HNSW_UPDATE_QT_TYPE : uint8 {
    UPDATE_NUM_NEW_DATA,
    UPDATE_CODE_VERSION,
    UPDATE_CENTROIDS_VERSION,
    UPDATE_CENTROIDS,
    WRITE_NEWCODE,
    BACKUP_SWAPCMD,
    SET_VALID
};

extern void HnswXLogUpdateMetaNumVector(Relation index, Buffer buf, Page page, size_t num_vector);
extern void HnswXLogUpdateMetaInsertPage(Relation index, Buffer buf, Page page, BlockNumber insert_blkno);
extern void HnswXLogUpdateMetaEntry(Relation index, Buffer buf, Page page, HnswEntryPointBase entry);
extern void HnswXLogUpdateNextBlkno(Relation index, Buffer buffer, BlockNumber blkno);
extern void HnswXLogAppendPage(Relation index, Buffer buffer, Page page);
extern void HnswXLogMarkDelete(Relation index, OffsetNumber offno, Buffer buf, Page page);
extern void HnswXLogAddVector(Relation index, char *value, off_t offset, int nbytes, VecStorageType vec_storage_type);
extern void hnsw_redo(XLogReaderState *record);
extern void hnsw_desc(StringInfo buf, XLogReaderState *record);
extern const char* hnsw_type_name(uint8 subtype);
extern void	HnswXLogBuildAddVector(Relation heap, Relation index, size_t vec_size, ForkNumber forkNum,
    BlockNumber metablkno, RaBitQParam &rbq_param);
extern void HnswXLogWriteNeighbor(Relation index, HnswNeighbor neighbor, int idx, OffsetNumber offno,
    Buffer buf, Page page);
extern void HnswXLogWriteNeighbors(Relation index, HnswNeighbor neighbor, uint32 n, OffsetNumber offno,
    Buffer buf, Page page);
extern void HnswXLogUpdateHeaptid(Relation index, ItemPointer heaptids, uint8 ntid, OffsetNumber offno,
    Buffer buf, Page page);
extern void HnswXLogAddElement(Relation index, Size tupSize, OffsetNumber freeOffno, OffsetNumber offno,
    Item tuple, Buffer buf, Page page);
/* update quantizer code */
extern void HnswXLogUpdateMetaNumNewData(Relation index, Buffer buf, Page page, uint32 num_new_data);
extern void HnswXLogWriteCentroids(Relation index, Buffer buf, Page page);
extern void HnswXlogWriteNewQtCode(Relation index, Page metapage, Buffer metabuf, uint32 file_idx, int nbytes, char *code);
extern void HnswXLogUpdateCodeVersion(Relation index, Buffer buf, Page page, uint8 code_version);
extern void HnswXLogUpdateCentroidsVersion(Relation index, Buffer buf, Page page, uint8 centroids_version);
extern void HnswXLogBackupSwapAndInvalidBufferCmd(Relation index, Page metapage, Buffer metabuf,
    uint32 file_idx, char *old_file_path, size_t invalid_size, QuantizerType qt_type);
extern void HnswRedoWriteNewCode(char *s);
extern void HnswRedoUpdateCentroids(char *centroids, Page page, size_t datalen);
extern void HnswRedoSwapVecFileAndInvalidBufferCmd(char *s);
extern void HnswXLogSetQTValid(Relation index, Page metapage, Buffer metabuf, bool valid);

struct xl_hnsw_overwrite_tuple {
	Size tupSize;
	OffsetNumber offno;
};

struct xl_hnsw_write_neighbor {
	HnswNeighborData data;
	uint16 idx;
	OffsetNumber offno;
};

struct xl_hnsw_add_element {
	Size tupSize;
	OffsetNumber freeOffno;
	OffsetNumber offno;
};

struct xl_hnsw_add_vec {
    off_t offset;
    RelFileNode r_Node;
    int nbytes;
	VecStorageType vec_storage_type;
};

struct xl_hnsw_tuple_base {
    enum Types : uint8 {
        NEIGHBORS,
        HEAP_TIDS,
        MARK_DELETE,
    };
    Types type;

    xl_hnsw_tuple_base(Types type_) : type(type_) {}
};

struct xl_hnsw_tuple_neighbors : public xl_hnsw_tuple_base {
    OffsetNumber offno;

    xl_hnsw_tuple_neighbors(OffsetNumber offno_)
        : xl_hnsw_tuple_base(Types::NEIGHBORS),
          offno(offno_) {}
};

struct xl_hnsw_tuple_heaptids : public xl_hnsw_tuple_base {
    OffsetNumber offno;

    xl_hnsw_tuple_heaptids(OffsetNumber offno_)
        : xl_hnsw_tuple_base(Types::HEAP_TIDS),
          offno(offno_) {}
};

struct xl_hnsw_tuple_mark_delete : public xl_hnsw_tuple_base {
    xl_hnsw_tuple_mark_delete() : xl_hnsw_tuple_base(Types::MARK_DELETE) {}
};

struct xl_hnsw_new_qtcode {
    Oid index_id;
    uint32 file_idx;
    int nbytes;
};

struct xl_hnsw_swapcmd {
    Oid index_id;
    uint32 file_idx;
    int len_old;

    /* clean standby buffer */
    Oid rel_node;
    size_t invalid_size;
    QuantizerType qt_type;
};

#endif /* HNSW_XLOG_H */
