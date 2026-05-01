/**
 * Copyright ...
 */

#ifndef SPARSEVECTOR_H
#define SPARSEVECTOR_H

#include "fmgr.h"

struct SparseVector {
    int32 vl_len_;  /* varlena header (do not touch directly!) */
    int32 unused;   /* pgvector compatible */
    uint32 nnz;     /* number of non-zero elements */
    int32 unused2;  /* reserved for future use, always zero */
    uint32 indices[FLEXIBLE_ARRAY_MEMBER];

    bool operator==(const SparseVector &other) const;
};

inline Size SPARSEVEC_SIZE(uint32 nnz)
    { return offsetof(SparseVector, indices) + (nnz * (sizeof(uint32) + sizeof(uint16))); }
inline SparseVector *COPY_SPARSEVEC(const SparseVector *sv)
{
    Size s = SPARSEVEC_SIZE(sv->nnz);
    SparseVector *res = (SparseVector *)palloc(s);
    errno_t rc = memcpy_s(res, s, sv, s);
    securec_check_c(rc, "\0", "\0");
    return res;
}
inline const uint16 *SPARSEVEC_VALUES(const SparseVector *x)
    { return (uint16 *)(((char *)x) + offsetof(SparseVector, indices) + (x->nnz * sizeof(uint32))); }
inline uint16 *SPARSEVEC_VALUES(SparseVector *x)
    { return (uint16 *)(((char *)x) + offsetof(SparseVector, indices) + (x->nnz * sizeof(uint32))); }

#define DatumGetSparseVector(x)     ((SparseVector *)PG_DETOAST_DATUM(x))
#define PG_GETARG_SPARSEVECTOR_P(x) DatumGetSparseVector(PG_GETARG_DATUM(x))
#define PG_RETURN_SPARSEVECTOR_P(x) PG_RETURN_POINTER(x)

/* recv and send */
extern Datum sparsevector_in(PG_FUNCTION_ARGS);
extern Datum sparsevector_out(PG_FUNCTION_ARGS);
extern Datum sparsevector_recv(PG_FUNCTION_ARGS);
extern Datum sparsevector_send(PG_FUNCTION_ARGS);

/* cmp */
extern Datum sparsevector_lt(PG_FUNCTION_ARGS);
extern Datum sparsevector_le(PG_FUNCTION_ARGS);
extern Datum sparsevector_eq(PG_FUNCTION_ARGS);
extern Datum sparsevector_ne(PG_FUNCTION_ARGS);
extern Datum sparsevector_ge(PG_FUNCTION_ARGS);
extern Datum sparsevector_gt(PG_FUNCTION_ARGS);
extern Datum sparsevector_cmp(PG_FUNCTION_ARGS);
extern Datum sparsevector_sortsupport(PG_FUNCTION_ARGS);

/* hash */
struct SparseVectorHasher {
    uint32 operator()(const SparseVector *a) const noexcept;
};
extern Datum hashsparsevector(PG_FUNCTION_ARGS);

/* ops */
/* get([10:2, 15:2], 10) == 2 */
extern Datum sparsevector_get(PG_FUNCTION_ARGS);
/* insert([10:2, 15:2], 11, 2) == [10:2, 11:2, 15:2] */
extern Datum sparsevector_insert(PG_FUNCTION_ARGS);
/* remove([10:2, 15:2], 10) == [15:2] */
extern Datum sparsevector_remove(PG_FUNCTION_ARGS);
/* multiply([10:2, 15:2], 2) == [10:4, 15:4] */
extern Datum sparsevector_multiply(PG_FUNCTION_ARGS);
extern Datum sparsevector_divide(PG_FUNCTION_ARGS);
/* merge([10:2, 15:2], [10:1, 11:1]) == [10:3, 11:1, 15:2] */
extern Datum sparsevector_merge(PG_FUNCTION_ARGS);
/* antimerge([10:2, 15:2], [10:1, 11:1]) == [10:1, 11:-1, 15:2] */
extern Datum sparsevector_antimerge(PG_FUNCTION_ARGS);

/* calc */
extern float sparsevector_norm(SparseVector *sv);
extern void sparsevector_normalize(SparseVector *a);
extern Datum sparsevector_l2_norm(PG_FUNCTION_ARGS);
extern Datum sparsevector_l2_normalize(PG_FUNCTION_ARGS);
extern Datum sparsevector_cosine_distance(PG_FUNCTION_ARGS);
extern Datum sparsevector_negative_inner_product(PG_FUNCTION_ARGS);

#endif /* SPARSEVECTOR_H */
