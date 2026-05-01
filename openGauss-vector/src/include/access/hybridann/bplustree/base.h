/**
 * Copyright ...
 * Utility section for B+ tree implementation.
 */

#ifndef DISKANN_CONTAINER_BPLUSTREE_BASE_H
#define DISKANN_CONTAINER_BPLUSTREE_BASE_H

#include "storage/off.h"
#include "access/tupdesc.h"
#include "utils/rel.h"
#include "utils/relcache.h"

namespace disk_container {

constexpr OffsetNumber p_hikey = 1u;
constexpr OffsetNumber p_firstkey = 2u;
constexpr uint16 LESS_THAN = 1u;
constexpr uint16 HYBRID_BTORDER_PROC = 2u;
#define p_firstdatakey(node) ((node).is_right_most() ? disk_container::p_hikey : disk_container::p_firstkey)

inline TupleDesc buildHybridTupDesc(Relation index)
{
    int nkeys = RelationGetDescr(index)->natts;
    if (nkeys  < 2) {
        elog(ERROR, "Number of index columns should >= 2");
    }
    TupleDesc tupleDesc = CreateTemplateTupleDesc(nkeys, false);
    TupleDescInitEntry(tupleDesc, (AttrNumber)1, "vector index", INT8OID, -1, 0);
    for (int i = 1; i < nkeys; ++i) {
        const FormData_pg_attribute &attr = RelationGetDescr(index)->attrs[i];
        TupleDescInitEntry(tupleDesc, (AttrNumber)(i + 1), NameStr(attr.attname), attr.atttypid,
                           attr.atttypmod, attr.attndims);
    }
    return tupleDesc;
}
} /* namespace disk_container */

#endif /* DISKANN_CONTAINER_BPLUSTREE_BASE_H */
