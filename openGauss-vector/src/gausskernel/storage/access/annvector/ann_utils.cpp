#include "access/annvector/ann_utils.h"
#include "access/heapam.h"
#include "utils/syscache.h"
#include "access/genam.h"
#include "catalog/pg_partition_fn.h"
#include "storage/smgr/smgr.h"

size_t get_relstats_reltuples(Relation rel)
{
    size_t reltuples_stats = 0;
    if (rel == NULL) {
        return reltuples_stats;
    }

    if (RelationIsPartition(rel)) {
        HeapTuple tuple = SearchSysCache1(PARTRELID, ObjectIdGetDatum(RelationGetRelid(rel)));
        if (HeapTupleIsValid(tuple)) {
            Form_pg_partition partition = (Form_pg_partition)GETSTRUCT(tuple);
            reltuples_stats = (size_t)partition->reltuples;
            ReleaseSysCache(tuple);
        }
    } else {
        HeapTuple tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(RelationGetRelid(rel)));
        if (HeapTupleIsValid(tuple)) {
            Form_pg_class pg_class_form = (Form_pg_class)GETSTRUCT(tuple);
            reltuples_stats = (size_t)pg_class_form->reltuples;
            ReleaseSysCache(tuple);
        }
    }

    return reltuples_stats;
}

void populate_index_partition_name(Relation index, char *indexName, char *partIndexName)
{
    sprintf(indexName, "%s", RelationGetRelationName(index));
    if (RelationIsPartition(index)) {
        Oid indexrelid = GetBaseRelOidOfParition(index);
        Oid indexpartid = RelationGetRelid(index);
        Relation indexRel = index_open(indexrelid, NoLock);
        Partition indexpart = partitionOpen(indexRel, indexpartid, NoLock);
        sprintf(partIndexName, "%s", PartitionGetPartitionName(indexpart));
        partitionClose(indexRel, indexpart, NoLock);
        index_close(indexRel, NoLock);
        RelationOpenSmgr(index);
    } else {
        partIndexName[0] = '\0';
    }
}

Buffer AnnNewBuffer(Relation index, ForkNumber forkNum)
{
    Buffer buf = ReadBufferExtended(index, forkNum, P_NEW, RBM_NORMAL, NULL);
    LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
    return buf;
}

void AnnCommitBuffer(Buffer buf)
{
    MarkBufferDirty(buf);
    UnlockReleaseBuffer(buf);
}

void check_ann_attributes(Relation index)
{
    Assert(RelationGetDescr(index)->natts >= 1);
    /* check whether the first or last attribute is FloatVector */
    if (TupleDescAttr(RelationGetDescr(index), 0)->atttypid != FLOATVECTOROID) {
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("The first attribute of index must be floatvector.")));
    }
    /* check all other attributes are not FloatVector */
    for (int i = 1; i < RelationGetDescr(index)->natts; ++i) {
        if (TupleDescAttr(RelationGetDescr(index), i)->atttypid == FLOATVECTOROID) {
            ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                            errmsg("Index can only store one floatvector attribute.")));
        }
    }
}


Buffer AnnLoadBuffer(Relation index, BlockNumber blkNo)
{
    Buffer buf = ReadBuffer(index, blkNo);
    LockBuffer(buf, BUFFER_LOCK_SHARE);
    return buf;
}

Buffer AnnLoadBufferExtended(Relation index, ForkNumber forkNum, BlockNumber blkNo)
{
    Buffer buf = ReadBufferExtended(index, forkNum, blkNo, RBM_NORMAL, NULL);
    LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
    return buf;
}

bool isHybridIndex(Relation index) { return RelationGetDescr(index)->natts > 1; }

/*
 * Divide by the norm
 *
 * Returns false if value should not be indexed
 *
 * The caller needs to free the pointer stored in value
 * if it's different than the original value
 */
bool AnnNormValue(ann_helper::distance_func procinfo, Datum *value, FloatVector *result)
{
	FloatVector *v = DatumGetFloatVector(*value);
	double norm = procinfo(v->x, v->x, v->dim);

	if (norm > 0) {
		if (result == NULL) {
			result = InitFloatVector(v->dim);
        }

		for (int i = 0; i < v->dim; ++i) {
			result->x[i] = v->x[i] / norm;
        }

		if ((Pointer)v != DatumGetPointer(*value)) {
			pfree(v);
		}		

		*value = PointerGetDatum(result);
		return true;
	}
	return false;
}

char *DatumGetVector(Datum value, DistPrecisionType type, Pointer *vec_out)
{
    char *vector = NULL;
    if (type == DistPrecisionType::FLOAT) {
        FloatVector *tempvec = DatumGetFloatVector(value);
        *vec_out = (Pointer)tempvec;
        vector = (char *)tempvec->x;
    } else {
        Assert(type == DistPrecisionType::HALF);
        HalfVector *tempvec = DatumGetHalfVector(value);
        *vec_out = (Pointer)tempvec;
        vector = (char *)tempvec->x;
    }
    return vector;
}
