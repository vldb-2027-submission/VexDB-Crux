#include "access/hybridann/hybridann.h"
#include "access/annvector/ann_utils.h"
#include "storage/buf/bufmgr.h"
#include "access/nbtree.h"

uint32 hybridAnnGetNumParallel(Relation index)
{
    HybridAnnOptions *opts = (HybridAnnOptions *) index->rd_options;
    return opts != NULL ? opts->parallel_workers : 0;
}

const char *hybridAnnGetVecIndexMagnitudes(Relation index)
{
    HybridAnnOptions *opts = (HybridAnnOptions *) index->rd_options;
    if (opts != NULL && opts->vec_index_magnitudes_offset > 0) {
        return (const char *)opts + opts->vec_index_magnitudes_offset;
    }
    return "";
}

int64 hybridAnnGetGraphMagnitudeThreshold(Relation index)
{
    HybridAnnOptions *opts = (HybridAnnOptions *) index->rd_options;
    return opts != NULL ? opts->graph_magnitude_threshold : 20000;
}


uint32 hybridannGetM(Relation index)
{
	HybridAnnOptions *opts = (HybridAnnOptions *) index->rd_options;

	if (opts)
		return opts->m;
	return 24;
}

uint32 hybridannGetEfConstruction(Relation index)
{
	HybridAnnOptions *opts = (HybridAnnOptions *) index->rd_options;
	if (opts)
		return opts->ef_construction;

	return 64;
}


int32 HybridAnnGetFillFactor(Relation index)
{
    HybridAnnOptions *opts = (HybridAnnOptions *) index->rd_options;
    return opts != NULL ? opts->fillfactor : BTREE_DEFAULT_FILLFACTOR;
}

/*
 * Get proc info
 */
FmgrInfo *hybridAnnOptionalProcInfo(Relation rel, uint16 procnum)
{
    if (!OidIsValid(index_getprocid(rel, 1, procnum))) {
        return NULL;
    }

    return index_getprocinfo(rel, 1, procnum);
}

void HybridAnnInitPage(Buffer buf, Page page)
{
    PageInit(page, BufferGetPageSize(buf), sizeof(HybridAnnPageOpaque));
    HybridAnnPageGetOpaque(page)->page_id = HYBRIDANN_PAGE_ID;
}

void hybridAnnInitMeta(Buffer buf, Page page)
{
    PageInit(page, BufferGetPageSize(buf), sizeof(HybridAnnPageOpaque));
    HybridAnnPageGetOpaque(page)->page_id = HYBRIDANN_META_ID;
}

BlockNumber hybridAnnGetFreespaceblkno(Relation index)
{
    Buffer buf = AnnLoadBuffer(index, HYBRIDANN_METAPAGE_BLKNO);
    HybridAnnMetaPage *meta = HybridAnnPageGetMeta(BufferGetPage(buf));
    if (unlikely(meta->magicNumber != HYBRIDANN_MAGIC_NUMBER)) {
        UnlockReleaseBuffer(buf);
        ereport(ERROR, (errcode(ERRCODE_INDEX_CORRUPTED), errmsg("Hybridann index is not valid")));
        return InvalidBlockNumber;
    }
    
    BlockNumber freespace_blkno = meta->freespaceMetaBlkNo;
    UnlockReleaseBuffer(buf);

    return freespace_blkno;
}


