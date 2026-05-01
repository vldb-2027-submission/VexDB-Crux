#include "postgres.h"
#include "fmgr.h"
#include "access/qasp/qasp.h"
#include "utils/builtins.h"
#include "utils/array.h"
#include "catalog/pg_type.h"
#include "catalog/namespace.h"
#include "catalog/index.h"

#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>

#ifdef PG_MODULE_MAGIC
PG_MODULE_MAGIC;
#endif

PG_FUNCTION_INFO_V1(qasp_manual_repair);
PG_FUNCTION_INFO_V1(qasp_repair_from_gt);

extern "C" {
    #include "access/annvector/floatvector.h"
}

extern "C" {
    Datum qasp_manual_repair(PG_FUNCTION_ARGS) {
        text *relname_text = PG_GETARG_TEXT_P(0);
        FloatVector *vec = PG_GETARG_FLOATVECTOR_P(1);
        int32 repair_ef = PG_GETARG_INT32(2);
    
        RangeVar *relvar = makeRangeVarFromNameList(textToQualifiedNameList(relname_text));
        Oid index_oid = RangeVarGetRelid(relvar, NoLock, false);
        Relation index_rel = index_open(index_oid, ShareUpdateExclusiveLock);
    
        int dim = vec->dim;
        float *vec_data = vec->x;
        if (dim <= 0 || dim > FLOATVECTOR_MAX_DIM) {
            index_close(index_rel, ShareUpdateExclusiveLock);
            ereport(ERROR, (errmsg("Invalid vector dimension: %d", dim)));
        }

        float *query_vec = (float *)palloc(sizeof(float) * dim);
        errno_t rc = memcpy_s(query_vec, sizeof(float) * dim, vec_data, sizeof(float) * dim);
        securec_check(rc, "\0", "\0");
    
        Buffer meta_buf = ReadBuffer(index_rel, QASP_METAPAGE_BLKNO);
        LockBuffer(meta_buf, BUFFER_LOCK_SHARE);
        QASPMetaPage *metaPage = QASPPageGetMeta(BufferGetPage(meta_buf));
    
        if (metaPage->magicNumber != QASP_MAGIC_NUMBER) {
            UnlockReleaseBuffer(meta_buf);
            index_close(index_rel, ShareUpdateExclusiveLock);
            pfree(query_vec);
            ereport(ERROR, (errmsg("Relation is not a valid QASP index")));
        }
        
        if (metaPage->dimensions != (uint32)dim) {
            UnlockReleaseBuffer(meta_buf);
            index_close(index_rel, ShareUpdateExclusiveLock);
            pfree(query_vec);
            ereport(ERROR, (errmsg("Query dimension mismatch")));
        }
    
        PG_TRY();
        {
            uint32 kNumGroundTruth = 100; 
            approxSingleQueryRepair(query_vec, metaPage, meta_buf, index_rel, kNumGroundTruth, (uint32)repair_ef);
        }
        PG_CATCH();
        {
            if (BufferIsValid(meta_buf)) UnlockReleaseBuffer(meta_buf);
            index_close(index_rel, ShareUpdateExclusiveLock);
            pfree(query_vec);
            PG_RE_THROW();
        }
        PG_END_TRY();
        UnlockReleaseBuffer(meta_buf);
    
        pfree(query_vec);
        index_close(index_rel, ShareUpdateExclusiveLock);
    
        PG_RETURN_BOOL(true);
    }

    Datum qasp_repair_from_gt(PG_FUNCTION_ARGS) {
        text *relname_text = PG_GETARG_TEXT_P(0);
        text *path_text = PG_GETARG_TEXT_P(1);
        int32 limit_k = PG_GETARG_INT32(2);
        
        char *filename = text_to_cstring(path_text);
        
        RangeVar *relvar = makeRangeVarFromNameList(textToQualifiedNameList(relname_text));
        Oid index_oid = RangeVarGetRelid(relvar, NoLock, false);
        Relation index_rel = index_open(index_oid, ShareUpdateExclusiveLock);
        
        Buffer meta_buf = ReadBuffer(index_rel, QASP_METAPAGE_BLKNO);
        LockBuffer(meta_buf, BUFFER_LOCK_SHARE);
        QASPMetaPage *metaPage = QASPPageGetMeta(BufferGetPage(meta_buf));
        
        if (metaPage->magicNumber != QASP_MAGIC_NUMBER) {
            UnlockReleaseBuffer(meta_buf);
            index_close(index_rel, ShareUpdateExclusiveLock);
            ereport(ERROR, (errmsg("Relation is not a valid QASP index")));
        }

        FILE *f = fopen(filename, "rb");
        if (!f) {
            UnlockReleaseBuffer(meta_buf);
            index_close(index_rel, ShareUpdateExclusiveLock);
            ereport(ERROR, (errmsg("Could not open GT file: %s", filename)));
        }
        
        fseek(f, 0, SEEK_END);
        size_t fileSize = ftell(f);
        fseek(f, 0, SEEK_SET);
        
        int fd = fileno(f);
        void *fmap = mmap(NULL, fileSize, PROT_READ, MAP_SHARED, fd, 0);
        if (fmap == MAP_FAILED) {
             fclose(f);
             UnlockReleaseBuffer(meta_buf);
             index_close(index_rel, ShareUpdateExclusiveLock);
             ereport(ERROR, (errmsg("mmap failed for GT file")));
        }
        
        uint32 ndata = *((uint32 *)fmap);
        uint32 k_neighbors = *((uint32 *)fmap + 1);
        int32 *data = (int32 *)((char*)fmap + 8); 
        
        elog(NOTICE, "Starting repair from GT. Queries: %u, Neighbors per query: %u", ndata, k_neighbors);

        PG_TRY();
        {
            for(uint32 i = 0; i < ndata; ++i) {
                CHECK_FOR_INTERRUPTS();
                int32 *row = data + i * (size_t)k_neighbors;
                uint32 current_k = k_neighbors; 
                if (limit_k > 0 && current_k > (uint32)limit_k) {
                    current_k = (uint32)limit_k;
                }
                repairSingleWithGT(index_rel, metaPage, 0, row, current_k);
                if ((i + 1) % 1000 == 0) {
                    elog(NOTICE, "Processed %u / %u queries...", i + 1, ndata);
                }
            }
        }
        PG_CATCH();
        {
            if (fmap != MAP_FAILED) munmap(fmap, fileSize);
            if (f) fclose(f);
            if (BufferIsValid(meta_buf)) UnlockReleaseBuffer(meta_buf);
            index_close(index_rel, ShareUpdateExclusiveLock);
            PG_RE_THROW();
        }
        PG_END_TRY();
        
        munmap(fmap, fileSize);
        fclose(f);
        UnlockReleaseBuffer(meta_buf);
        index_close(index_rel, ShareUpdateExclusiveLock);
        
        elog(NOTICE, "GT Repair completed.");
        PG_RETURN_BOOL(true);
    }
}
