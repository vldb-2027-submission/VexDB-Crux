/**
 * Copyright ...
 * Float vector store and access interface.
 * There are two sections in this file:
 *  1. Storage: store & read vector to relation files with VECTOR_FORNUM.
 *  2. Access: a read-only buffer and its backend flusher threads for vectors.
 */

#ifndef VECTOR_SMGR_H
#define VECTOR_SMGR_H

#include <vtl/definition>
#include "storage/smgr/smgr.h"
#include "access/annvector/store/buffer_base.h"

#define VERIFY_BUFFER false

struct BufferParams {
    Relation rel;
    size_t loc;
    size_t elem_size; /* elem_size is the size of cache element */
    int16 pool_offset;
    VecStorageType storage_type;
    /* read pos */
    uint32 buf_offset;
    uint32 offset;
    SMGR_READ_STATUS status;
};

struct VecBufferLoc : SAFE_CONSTRUCTOR {
    uint32 buf_offset;
    uint32 offset;
    VecBufferLoc() {}
    VecBufferLoc(uint32 buf_offset, uint32 offset) : buf_offset(buf_offset), offset(offset) {}
    VecBufferLoc(BufferParams &);
    bool operator==(const VecBufferLoc &rhs) const
    {
        uint64 x = (uint64(buf_offset) << 32) | offset;
        uint64 y = (uint64(rhs.buf_offset) << 32) | rhs.offset;
        return x == y;
    }
    bool operator<(const VecBufferLoc &rhs) const
    {
        uint64 x = (uint64(buf_offset) << 32) | offset;
        uint64 y = (uint64(rhs.buf_offset) << 32) | rhs.offset;
        return x < y;
    }

    static constexpr uint32 invalid_mask = 0x80000000u;
    static constexpr uint32 empty_mask = 0x7fffffffu;
    bool valid() const { return !(buf_offset & invalid_mask); }
    bool empty() const { return (buf_offset & empty_mask) == empty_mask; }
    uint32 valid_buf_offset() const { return buf_offset & (~invalid_mask); }
    void set_valid() { buf_offset = valid_buf_offset(); }
    void set_invalid() { buf_offset |= invalid_mask; }
    void set_empty() { buf_offset |= empty_mask; }
};

struct VecBuffer {
#if VERIFY_BUFFER
    Oid rel_id;
#endif /* VERIFY_BUFFER */
    int16 pool_offset;
    VecBufferLoc loc;
    char *buf;

    VecBuffer() {}
#if VERIFY_BUFFER
    VecBuffer(Relation rel, int16 pool_offset, uint32 buf_offset, uint32 offset, float *buf)
        : rel_id(rel->rd_smgr->smgr_rnode.node.relNode),
          pool_offset(pool_offset),
          loc(buf_offset, offset),
          buf(buf) {}
#else
    VecBuffer(int16 pool_offset, uint32 buf_offset, uint32 offset, char *buf)
        : pool_offset(pool_offset),
          loc(buf_offset, offset),
          buf(buf) {}
#endif /* VERIFY_BUFFER */
    char *get_vecbuf() { return buf; }
    void release();
};

struct VectorBufferInspect {
    char *used_space;
    char *elem_size;
    size_t elem_nums;
    size_t hit;
    size_t miss;
    size_t evict;
};

extern void init_vector_smgr();
extern VecBuffer vec_read_buffer(Relation rel, size_t loc, size_t vec_size, VecStorageType st = VecStorageType::PureVec);
extern VecBuffer vec_read_quant(Relation rel, size_t loc, size_t code_len, VecStorageType st);
extern void vec_invalidate_buffer_cache(Oid relNode, size_t loc, size_t elem_size);
extern void vec_invalidate_buffer_cache(Oid relNode, size_t elem_size);
extern void release_vector_buffer(const VecBufferLoc &loc);
extern void vec_buffer_report_stats(int err_level, uint32 dim, bool reset = false);
extern size_t vec_buffer_verify(uint32 dim, size_t &total_slot);
extern bool enable_vec_buffer_manager();
extern void vec_writer_main(void);
extern void copy_vector_file(Relation rel, SMgrRelation *dstptr, char relpersistence);
extern void truncate_vector_file(Relation rel);
extern int pread_file(File file, char *buffer, int amount, off_t offset);
extern void create_vec_data(Relation rel, bool need_wal);
extern SMGR_READ_STATUS vec_read(SMgrRelation reln, off_t offset, int nbytes, char *buffer,
                                 VecStorageType vec_storage_type = VecStorageType::PureVec);

extern void vec_write(SMgrRelation reln, off_t offset, int nbytes, const char *buffer,
                      bool skip_fsync, VecStorageType vec_storage_type = VecStorageType::PureVec);

extern void read_vector(Relation rel, size_t loc, size_t dim, char *vec, bool with_code = false);
SMGR_READ_STATUS read_vector_no_error(Relation rel, size_t loc, size_t elem_size, char *vec, bool with_code = false);
extern void write_vector(Relation rel, size_t loc, size_t vec_size, const char *vec, bool with_code = false);

extern void read_qtcode(Relation rel, size_t loc, size_t code_len, char *code, bool with_vec = true);
SMGR_READ_STATUS read_qtcode_no_error(Relation rel, size_t loc, size_t code_len, char *code, bool with_vec = true);
extern void write_qtcode(Relation rel, size_t loc, size_t code_len, const char *code, bool with_vec = true);
extern Datum vectorbuffer_inspect(PG_FUNCTION_ARGS);

#endif /* VECTOR_SMGR_H */
