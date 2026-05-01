#ifndef BUFFER_MANAGER_H
#define BUFFER_MANAGER_H

#include "postgres.h"
#include "storage/smgr/relfilenode.h"
#include "utils/fmgroids.h"
#include "access/annvector/store/bulkbuf_smgr.h"
#include "access/annvector/store/vector_smgr.h"

inline void init_vector_engine_buffer_manager() {
    init_bulkbuf_smgr();
    init_vector_smgr();
}

#define BULKBUF_SUPPORT(index) ((index)->rd_am->ambuild == F_HNSWBUILD)
#define GET_BULKBUF(index) BULKBUF_MGR->get_bulkbuf(index)

#endif /* BUFFER_MANAGER_H */