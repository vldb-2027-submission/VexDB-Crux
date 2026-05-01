#include "postgres.h"
#include "fmgr.h"
#include "funcapi.h"
#include "libpq/pqformat.h"
#include "utils/builtins.h"
#include "access/hash.h"
#include "access/annvector/sparsevector.h"
#include "access/annvector/floatvector.h"
#include "access/annvector/halfutils.h"

#ifndef HALF_SHORTEST_DECIMAL_LEN
#define HALF_SHORTEST_DECIMAL_LEN 11
#endif

struct SparseInputElement {
    uint32 index;
    float value;
};

static bool sparsevec_isspace(char ch)
{
    if (ch == ' ' || ch == '\t' || ch == '\n' ||
        ch == '\r' || ch == '\v' || ch == '\f') {
        return true;
    }
    return false;
}

static void skip_space(char *&pt)
{
    while (sparsevec_isspace(*pt)) {
        ++pt;
    }
}

bool SparseVector::operator==(const SparseVector &other) const
{
    if (nnz != other.nnz) {
        return false;
    }
    const uint32 *other_indices = other.indices;
    const uint16 *other_values = SPARSEVEC_VALUES(&other);
    const uint16 *this_values = SPARSEVEC_VALUES(this);
    for (uint32 i = 0; i < nnz; ++i) {
        if (indices[i] != other_indices[i]) {
            return false;
        }
        if (this_values[i] != other_values[i]) {
            return false;
        }
    }
    return true;
}

SparseVector *InitSparseVector(uint32 nnz)
{
    Size size = SPARSEVEC_SIZE(nnz);
    SparseVector *result = (SparseVector *)palloc0(size);
    SET_VARSIZE(result, size);
    result->unused = 0;
    result->nnz = nnz;
    result->unused2 = 1u;
    return result;
}

static int CompareIndices(const void *a, const void *b)
{
    if (((SparseInputElement *)a)->index < ((SparseInputElement *)b)->index) {
        return -1;
    }
    if (((SparseInputElement *)a)->index > ((SparseInputElement *)b)->index) {
        return 1;
    }
    return 0;
}

static void CheckElement(float value)
{
    if (isnan(value)) {
        ereport(ERROR,
            (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
             errmsg("NaN not allowed in sparsevector")));
    }
    if (isinf(value)) {
        ereport(ERROR,
            (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
             errmsg("infinite value not allowed in sparsevector")));
    }
    if (value > HALF_MAX) {
        ereport(ERROR,
            (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
             errmsg("sparsevector value %f is not allowed to be greater than %f", value, HALF_MAX)));
    }
    if (value < -HALF_MAX) {
        ereport(ERROR,
            (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
             errmsg("sparsevector value %f is not allowed to be less than %f", value, -HALF_MAX)));
    }
}

static void CheckIndex(unsigned long index)
{
    if (index == 0) {
        ereport(ERROR,
            (errcode(ERRCODE_DATA_EXCEPTION),
             errmsg("index not allowed smaller than 1 in sparsevector")));
    }
    if (index > UINT32_MAX) {
        ereport(ERROR,
            (errcode(ERRCODE_DATA_EXCEPTION),
             errmsg("index not allowed bigger than %u in sparsevector", UINT32_MAX)));
    }
}

/* recv and send */
Datum sparsevector_in(PG_FUNCTION_ARGS)
{
    char *lit = PG_GETARG_CSTRING(0);
    char *pt = lit;
    char *stringEnd;
    
    uint32 maxNnz = 1;
    uint32 nnz = 0;
    for (; *pt != '\0'; ++pt) {
        if (*pt == ',') {
            ++maxNnz;
        }
    }
    SparseInputElement *elements = (SparseInputElement *)palloc(maxNnz * sizeof(SparseInputElement));

    pt = lit;
    skip_space(pt);
    if (*pt != '{' && *pt != '[') {
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
                 errmsg("invalid input syntax for type sparsevector: \"%s\"", lit),
                 errdetail("Sparsvector contents must start with \"[\" or \"{\".")));
    }
    const char closing_bracket = *pt == '{' ? '}' : ']';
    ++pt;
    skip_space(pt);
    if (*pt == closing_bracket) {
        ++pt;
    } else {
        for (;;) {
            if (nnz == maxNnz) {
                ereport(ERROR,
                        (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
                         errmsg("ran out of buffer: \"%s\"", lit)));
            }
            skip_space(pt);
            /* Check for empty string like float4in */
            if (*pt == '\0') {
                ereport(ERROR,
                        (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
                         errmsg("invalid input syntax for type sparsevector: \"%s\"", lit)));
            }
            unsigned long index = strtoul(pt, &stringEnd, 10);
            if (stringEnd == pt) {
                ereport(ERROR,
                        (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
                         errmsg("invalid input syntax for type sparsevector: \"%s\"", lit)));
            }
            CheckIndex(index);
            pt = stringEnd;
            skip_space(pt);
            if (*pt != ':') {
                ereport(ERROR,
                        (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
                         errmsg("invalid input syntax for type sparsevector: \"%s\"", lit)));
            }
            ++pt;
            skip_space(pt);
            float value = strtof(pt, &stringEnd);
            if (stringEnd == pt) {
                ereport(ERROR,
                        (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
                         errmsg("invalid input syntax for type sparsevector: \"%s\"", lit)));
            }
            CheckElement(value);
            if (value != 0) {
                elements[nnz] = {(uint32)index, value};
                ++nnz;
            }
            pt = stringEnd;
            skip_space(pt);
            if (*pt == ',') {
                ++pt;
            } else if (*pt == closing_bracket) {
                ++pt;
                break;
            } else {
                ereport(ERROR,
                        (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
                         errmsg("invalid input syntax for type sparsevector: \"%s\"", lit)));
            }
        }
    }
    skip_space(pt);
    /* try extract dim info, just for compatibility with pgvector */
    if (*pt == '/') {
        ++pt;
        skip_space(pt);
        strtol(pt, &stringEnd, 10);
        if (stringEnd == pt) {
            ereport(ERROR,
                    (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
                     errmsg("invalid input syntax for type sparsevector: \"%s\"", lit)));
        }
        pt = stringEnd;
    }

    /* Only whitespace is allowed after the closing brace */
    skip_space(pt);
    if (*pt != '\0') {
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
                 errmsg("invalid input syntax for type sparsevector: \"%s\"", lit),
                 errdetail("Junk after closing.")));
    }

    qsort(elements, nnz, sizeof(SparseInputElement), CompareIndices);
    for (uint32 i = 1u; i < nnz; ++i) {
        if (elements[i].index == elements[i - 1u].index) {
            ereport(ERROR,
                (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
                 errmsg("Found duplicate dimensions %u in sparsevector: \"%s\"",
                        elements[i].index, lit)));
        }
    }

    SparseVector *result = InitSparseVector(nnz);
    uint16 *rvalues = SPARSEVEC_VALUES(result);
    for (uint32 i = 0; i < nnz; ++i) {
        result->indices[i] = elements[i].index;
        rvalues[i] = float_to_half(elements[i].value);
    }

    PG_RETURN_POINTER(result);
}

int half_to_shortest_decimal_bufn(uint16 f, char *buf)
{
    int len = snprintf(buf, HALF_SHORTEST_DECIMAL_LEN, "%.4g", half_to_float(f));
    return std::min(len, HALF_SHORTEST_DECIMAL_LEN - 1);
}

#define AppendChar(ptr, c) (*(ptr)++ = (c))
#define AppendHalf(ptr, f) ((ptr) += half_to_shortest_decimal_bufn((f), (ptr)))
#define AppendUint32(ptr, i) \
    do { \
        ptr = pg_ultostr(ptr, i); \
        *ptr = '\0'; \
    } while (0)

Datum sparsevector_out(PG_FUNCTION_ARGS)
{
    SparseVector *sparsevector = PG_GETARG_SPARSEVECTOR_P(0);
    uint16 *values = SPARSEVEC_VALUES(sparsevector);
    /*
     * Need:
     *
     * nnz * 10 bytes for index (positive integer)
     *
     * nnz bytes for :
     *
     * nnz * (HALF_SHORTEST_DECIMAL_LEN - 1) bytes for
     * float_to_shortest_decimal_bufn
     *
     * nnz - 1 bytes for ,
     *
     * 3 bytes for {, } and \0
     */
    char *buf = (char *)palloc((11 + HALF_SHORTEST_DECIMAL_LEN) * sparsevector->nnz + 3);
    char *ptr = buf;
    AppendChar(ptr, '[');
    for (uint32 i = 0; i < sparsevector->nnz; ++i) {
        if (i > 0) {
            AppendChar(ptr, ',');
        }
        AppendUint32(ptr, sparsevector->indices[i]);
        AppendChar(ptr, ':');
        AppendHalf(ptr, values[i]);
    }
    AppendChar(ptr, ']');
    *ptr = '\0';

    PG_FREE_IF_COPY(sparsevector, 0);
    PG_RETURN_CSTRING(buf);
}

Datum sparsevector_recv(PG_FUNCTION_ARGS)
{
    StringInfo buf = (StringInfo)PG_GETARG_POINTER(0);
    pq_getmsgint(buf, sizeof(uint32));  /* unused */
    int _nnz = pq_getmsgint(buf, sizeof(uint32));
    if (_nnz < 0) {
        ereport(ERROR,
            (errcode(ERRCODE_DATA_EXCEPTION),
                errmsg("sparsevector cannot have negative number of elements")));
    }
    uint32 nnz = (uint32)_nnz;
    SparseVector *result = InitSparseVector(nnz);
    uint16 *values = SPARSEVEC_VALUES(result);
    uint32 formatted = result->unused2 = pq_getmsgint(buf, sizeof(uint32));

    for (uint32 i = 0; i < nnz; ++i) {
        result->indices[i] = pq_getmsgint(buf, sizeof(uint32));
    }

    if (formatted == 0) {
        for (uint32 i = 0; i < nnz; ++i) {
            float temp = pq_getmsgfloat4(buf);
            CheckElement(temp);
            values[i] = float_to_half(temp);
        }
        // TD: check sort, index range, value range, zero value and duplicates
    } else {
        for (uint32 i = 0; i < nnz; ++i) {
            float temp = pq_getmsgfloat4(buf);
            values[i] = float_to_half(temp);
        }
    }

    PG_RETURN_POINTER(result);
}

Datum sparsevector_send(PG_FUNCTION_ARGS)
{
    SparseVector *svec = PG_GETARG_SPARSEVECTOR_P(0);
    uint16 *values = SPARSEVEC_VALUES(svec);
    StringInfoData buf;

    pq_begintypsend(&buf);
    pq_sendint(&buf, svec->unused, sizeof(uint32));
    pq_sendint(&buf, svec->nnz, sizeof(uint32));
    pq_sendint(&buf, 1u, sizeof(uint32));

    for (uint32 i = 0; i < svec->nnz; ++i) {
        pq_sendint(&buf, svec->indices[i], sizeof(uint32));
    }
    for (uint32 i = 0; i < svec->nnz; i++) {
        pq_sendfloat4(&buf, half_to_float(values[i]));
    }

    PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}

static int sparsevec_cmp_internal(const SparseVector *a, const SparseVector *b)
{
    const uint16 *ax = SPARSEVEC_VALUES(a);
    const uint16 *bx = SPARSEVEC_VALUES(b);
    uint32 nnz = std::min(a->nnz, b->nnz);

    /* Only check values, be consistent with Postgres arrays */
    for (uint32 i = 0; i < nnz; i++) {
        if (a->indices[i] < b->indices[i]) {
            return ax[i] < 0 ? -1 : 1;
        }
        if (a->indices[i] > b->indices[i]) {
            return bx[i] < 0 ? 1 : -1;
        }
        float af = half_to_float(ax[i]);
        float bf = half_to_float(bx[i]);
        if (af < bf) {
            return -1;
        }
        if (af > bf) {
            return 1;
        }
    }
    return (int)a->nnz - (int)b->nnz;
}

/* cmp */
Datum sparsevector_lt(PG_FUNCTION_ARGS)
{
    SparseVector *a = PG_GETARG_SPARSEVECTOR_P(0);
    SparseVector *b = PG_GETARG_SPARSEVECTOR_P(1);
    PG_RETURN_BOOL(sparsevec_cmp_internal(a, b) < 0);
}

Datum sparsevector_le(PG_FUNCTION_ARGS)
{
    SparseVector *a = PG_GETARG_SPARSEVECTOR_P(0);
    SparseVector *b = PG_GETARG_SPARSEVECTOR_P(1);
    PG_RETURN_BOOL(sparsevec_cmp_internal(a, b) <= 0);
}

Datum sparsevector_eq(PG_FUNCTION_ARGS)
{
    SparseVector *a = PG_GETARG_SPARSEVECTOR_P(0);
    SparseVector *b = PG_GETARG_SPARSEVECTOR_P(1);
    PG_RETURN_BOOL(sparsevec_cmp_internal(a, b) == 0);
}

Datum sparsevector_ne(PG_FUNCTION_ARGS)
{
    SparseVector *a = PG_GETARG_SPARSEVECTOR_P(0);
    SparseVector *b = PG_GETARG_SPARSEVECTOR_P(1);
    PG_RETURN_BOOL(sparsevec_cmp_internal(a, b) != 0);
}

Datum sparsevector_ge(PG_FUNCTION_ARGS)
{
    SparseVector *a = PG_GETARG_SPARSEVECTOR_P(0);
    SparseVector *b = PG_GETARG_SPARSEVECTOR_P(1);
    PG_RETURN_BOOL(sparsevec_cmp_internal(a, b) >= 0);
}

Datum sparsevector_gt(PG_FUNCTION_ARGS)
{
    SparseVector *a = PG_GETARG_SPARSEVECTOR_P(0);
    SparseVector *b = PG_GETARG_SPARSEVECTOR_P(1);
    PG_RETURN_BOOL(sparsevec_cmp_internal(a, b) > 0);
}

Datum sparsevector_cmp(PG_FUNCTION_ARGS)
{
    SparseVector *a = PG_GETARG_SPARSEVECTOR_P(0);
    SparseVector *b = PG_GETARG_SPARSEVECTOR_P(1);
    PG_RETURN_INT32(sparsevec_cmp_internal(a, b));
}

static int sparsevector_fastcmp(Datum x, Datum y, SortSupport)
{
    SparseVector *a = (SparseVector *)DatumGetSparseVector(x);
    SparseVector *b = (SparseVector *)DatumGetSparseVector(y);
    return sparsevec_cmp_internal(a, b);
}

Datum sparsevector_sortsupport(PG_FUNCTION_ARGS)
{
    SortSupport ssup = (SortSupport)PG_GETARG_POINTER(0);
    ssup->comparator = sparsevector_fastcmp;
    PG_RETURN_VOID();
}

/* hash */
uint32 SparseVectorHasher::operator()(const SparseVector *a) const noexcept
{
    return DatumGetUInt32(
        hash_any((unsigned char *)a->indices, (sizeof(uint32) + sizeof(uint16)) * a->nnz));
}
Datum hashsparsevector(PG_FUNCTION_ARGS)
{
    SparseVector *a = (SparseVector *)PG_GETARG_SPARSEVECTOR_P(0);
    return SparseVectorHasher()(a);
}

/* ops */
Datum sparsevector_get(PG_FUNCTION_ARGS)
{
    SparseVector *a = (SparseVector *)PG_GETARG_SPARSEVECTOR_P(0);
    uint32 k = (uint32)PG_GETARG_INT32(1);
    CheckIndex(k);
    uint16 *values = SPARSEVEC_VALUES(a);
    float4 res = 0.0;
    for (uint32 i = 0; i < a->nnz; ++i) {
        if (a->indices[i] == k) {
            res = half_to_float(values[i]);
            break;
        }
    }
    PG_RETURN_FLOAT4(res);
}

Datum sparsevector_insert(PG_FUNCTION_ARGS)
{
    SparseVector *a = (SparseVector *)PG_GETARG_SPARSEVECTOR_P(0);
    uint32 k = (uint32)PG_GETARG_INT32(1);
    CheckIndex(k);
    float v = PG_GETARG_FLOAT4(2);
    uint16 *values = SPARSEVEC_VALUES(a);

    bool dupliate = false;
    uint32 new_nnz = a->nnz + 1;
    for (uint32 i = 0; i < a->nnz; ++i) {
        uint32 k_a = a->indices[i];
        if (k_a == k) {
            dupliate = true;
            float v_a = half_to_float(values[i]);
            if (v_a + v == 0) {
                new_nnz = a->nnz - 1;
            } else {
                new_nnz = a->nnz;
            }
        }
    }

    SparseVector *result = InitSparseVector(new_nnz);
    uint16 *rvalues = SPARSEVEC_VALUES(result);
    
    bool added = false;
    uint32 r = 0;
    for (uint32 i = 0; i < a->nnz;) {
        uint32 k_a = a->indices[i];
        if (added || k_a < k) {
            result->indices[r] = k_a;
            rvalues[r] = values[i];
            ++i;
            ++r;
        } else if (k_a > k) {
            result->indices[r] = k;
            rvalues[r] = v;
            added = true;
            ++r;
        } else {
            float sum = v + half_to_float(values[i]);
            if (sum != 0) {
                result->indices[r] = k;
                rvalues[r] = float_to_half(sum);
                ++r;
            }
            added = true;
            ++i;
        }
    }
    if (!added) {
        result->indices[r] = k;
        rvalues[r] = float_to_half(v);
    }
    PG_RETURN_SPARSEVECTOR_P(result);
}

Datum sparsevector_remove(PG_FUNCTION_ARGS)
{
    SparseVector *a = (SparseVector *)PG_GETARG_SPARSEVECTOR_P(0);
    uint32 k = (uint32)PG_GETARG_INT32(1);
    CheckIndex(k);
    uint16 *values = SPARSEVEC_VALUES(a);

    bool find = false;
    uint32 new_nnz = a->nnz - 1;
    for (uint32 i = 0; i < a->nnz; ++i) {
        if (a->indices[i] == k) {
            find = true;
            continue;
        }
    }
    if (!find) {
        PG_RETURN_SPARSEVECTOR_P(a);
    }

    SparseVector *result = InitSparseVector(new_nnz);
    uint16 *rvalues = SPARSEVEC_VALUES(result);

    for (uint32 i = 0, r = 0; i < a->nnz; ++i, ++r) {
        if (a->indices[i] == k) {
            --r;
            continue;
        }
        result->indices[r] = a->indices[i];
        rvalues[r] = values[i];
    }
    PG_RETURN_SPARSEVECTOR_P(result);
}

Datum sparsevector_multiply(PG_FUNCTION_ARGS)
{
    SparseVector *a = (SparseVector *)PG_GETARG_SPARSEVECTOR_P(0);
    float factor = PG_GETARG_FLOAT4(1);
    uint16 *values = SPARSEVEC_VALUES(a);
    if (factor == 0) {
        SparseVector *result = InitSparseVector(0);
        PG_RETURN_SPARSEVECTOR_P(result);
    }

    SparseVector *result = InitSparseVector(a->nnz);
    uint16 *rvalues = SPARSEVEC_VALUES(result);

    for (uint32 i = 0; i < a->nnz; ++i) {
        result->indices[i] = a->indices[i];
    }
    if (factor > 1) {
        for (uint32 i = 0; i < a->nnz; ++i) {
            float temp = half_to_float(values[i]) * factor;
            CheckElement(temp);
            rvalues[i] = float_to_half(temp);
        }
    } else {
        for (uint32 i = 0; i < a->nnz; ++i) {
            rvalues[i] = float_to_half(half_to_float(values[i]) / factor);
        }
    }
    PG_RETURN_SPARSEVECTOR_P(result);
}

Datum sparsevector_divide(PG_FUNCTION_ARGS)
{
    SparseVector *a = (SparseVector *)PG_GETARG_SPARSEVECTOR_P(0);
    float factor = PG_GETARG_FLOAT4(1);
    uint16 *values = SPARSEVEC_VALUES(a);
    if (factor == 0) {
        ereport(ERROR,
            (errcode(ERRCODE_DIVISION_BY_ZERO),
             errmsg("division by zero")));
    }
    factor = 1 / factor;

    SparseVector *result = InitSparseVector(a->nnz);
    uint16 *rvalues = SPARSEVEC_VALUES(result);

    for (uint32 i = 0; i < a->nnz; ++i) {
        result->indices[i] = a->indices[i];
    }
    if (factor > 1) {
        for (uint32 i = 0; i < a->nnz; ++i) {
            float temp = half_to_float(values[i]) * factor;
            CheckElement(temp);
            rvalues[i] = float_to_half(temp);
        }
    } else {
        for (uint32 i = 0; i < a->nnz; ++i) {
            rvalues[i] = float_to_half(half_to_float(values[i]) / factor);
        }
    }
    PG_RETURN_SPARSEVECTOR_P(result);
}

Datum sparsevector_merge(PG_FUNCTION_ARGS)
{
    SparseVector *a = (SparseVector *)PG_GETARG_SPARSEVECTOR_P(0);
    SparseVector *b = (SparseVector *)PG_GETARG_SPARSEVECTOR_P(1);
    uint16 *ax = SPARSEVEC_VALUES(a);
    uint16 *bx = SPARSEVEC_VALUES(b);

    uint32 new_nnz = a->nnz + b->nnz;
    SparseVector *result = InitSparseVector(new_nnz);
    uint16 *rx = SPARSEVEC_VALUES(result);
    uint32 i = 0, j = 0, r = 0;
    uint32 k1 = 0;
    uint32 k2 = 0;
    for (; i < a->nnz && j < b->nnz; ++r) {
        k1 = a->indices[i];
        k2 = b->indices[j];
        uint16 v1 = ax[i];
        uint16 v2 = bx[j];
        if (k1 < k2) {
            result->indices[r] = k1;
            rx[r] = v1;
            ++i;
        } else if (k1 > k2) {
            result->indices[r] = k2;
            rx[r] = v2;
            ++j;
        } else {
            float sum = half_to_float(v1) + half_to_float(v2);
            if (sum != 0) {
                result->indices[r] = k1;
                rx[r] = float_to_half(sum);
                new_nnz -= 1;
            } else {
                new_nnz -= 2;
                --r;
            }
            ++i;
            ++j;
        }
    }
    if (i < a->nnz) {
        for (; i < a->nnz; ++i, ++r) {
            result->indices[r] = a->indices[i];
            rx[r] = ax[i];
        }
    } else if (j < b->nnz) {
        for (; j < b->nnz; ++j, ++r) {
            result->indices[r] = b->indices[i];
            rx[r] = bx[i];
        }
    }

    PG_FREE_IF_COPY(a, 0);
    PG_FREE_IF_COPY(b, 1);

    result->nnz = r;
    SET_VARSIZE(result, SPARSEVEC_SIZE(r));
    PG_RETURN_SPARSEVECTOR_P(result);
}

Datum sparsevector_antimerge(PG_FUNCTION_ARGS)
{
    SparseVector *a = (SparseVector *)PG_GETARG_SPARSEVECTOR_P(0);
    SparseVector *b = (SparseVector *)PG_GETARG_SPARSEVECTOR_P(1);
    uint16 *ax = SPARSEVEC_VALUES(a);
    uint16 *bx = SPARSEVEC_VALUES(b);

    uint32 new_nnz = a->nnz + b->nnz;
    SparseVector *result = InitSparseVector(new_nnz);
    uint16 *rx = SPARSEVEC_VALUES(result);
    uint32 i = 0, j = 0, r = 0;
    uint32 k1 = 0;
    uint32 k2 = 0;
    for (; i < a->nnz && j < b->nnz; ++r) {
        k1 = a->indices[i];
        k2 = b->indices[j];
        uint16 v1 = ax[i];
        uint16 v2 = bx[j];
        if (k1 < k2) {
            result->indices[r] = k1;
            rx[r] = v1;
            ++i;
        } else if (k1 > k2) {
            result->indices[r] = k2;
            rx[r] = -v2;
            ++j;
        } else {
            float sum = half_to_float(v1) - half_to_float(v2);
            if (sum != 0) {
                result->indices[r] = k1;
                rx[r] = float_to_half(sum);
                new_nnz -= 1;
            } else {
                new_nnz -= 2;
                --r;
            }
            ++i;
            ++j;
        }
    }
    if (i < a->nnz) {
        for (; i < a->nnz; ++i, ++r) {
            result->indices[r] = a->indices[i];
            rx[r] = ax[i];
        }
    } else if (j < b->nnz) {
        for (; j < b->nnz; ++j, ++r) {
            result->indices[r] = b->indices[j];
            rx[r] = -bx[j];
        }
    }

    PG_FREE_IF_COPY(a, 0);
    PG_FREE_IF_COPY(b, 1);

    result->nnz = r;
    SET_VARSIZE(result, SPARSEVEC_SIZE(r));
    PG_RETURN_SPARSEVECTOR_P(result);
}

/* calc */
static float SparsevecInnerProduct(const SparseVector * a, const SparseVector * b)
{
    const uint16 *ax = SPARSEVEC_VALUES(a);
    const uint16 *bx = SPARSEVEC_VALUES(b);
    float distance = 0.0;
    uint32 i = 0, j = 0;
    while (i < a->nnz && j < b->nnz) {
        uint32 ai = a->indices[i];
        uint32 bi = b->indices[j];
        if (ai < bi) {
            ++i;
        } else if (ai > bi) {
            ++j;
        } else {
            distance += half_to_float(ax[i]) * half_to_float(bx[j]);
            ++i;
            ++j;
        }
    }
    return distance;
}

float sparsevector_norm(SparseVector *a)
{
    uint16 *ax = SPARSEVEC_VALUES(a);
    double norm = 0.0;
    for (uint32 i = 0; i < a->nnz; ++i) {
        float temp = half_to_float(ax[i]);
        norm += temp * temp;
    }
    return sqrt(norm);
}

void sparsevector_normalize(SparseVector *a)
{
    if (unlikely(a->nnz == 0)) {
        return;
    }
    float *buf = (float *)palloc(sizeof(float) * a->nnz);
    uint16 *ax = SPARSEVEC_VALUES(a);
    double norm = 0.0;
    for (uint32 i = 0; i < a->nnz; ++i) {
        float temp = buf[i] = half_to_float(ax[i]);
        norm += temp * temp;
    }
    norm = sqrt(norm);
    uint32 offset = 0;
    for (uint32 i = 0; i < a->nnz; ++i) {
        uint16 temp = float_to_half(buf[i] / norm);
        if (temp == 0) {
            continue;
        }
        ax[offset] = temp;
        ++offset;
    }
    pfree(buf);
    if (offset != a->nnz) {
        a->nnz = offset;
        SET_VARSIZE(a, SPARSEVEC_SIZE(offset));
    }
}

Datum sparsevector_l2_norm(PG_FUNCTION_ARGS)
{
    PG_RETURN_FLOAT4(sparsevector_norm(PG_GETARG_SPARSEVECTOR_P(0)));
}

Datum sparsevector_l2_normalize(PG_FUNCTION_ARGS)
{
    SparseVector *a = PG_GETARG_SPARSEVECTOR_P(0);
    uint16 *ax = SPARSEVEC_VALUES(a);
    SparseVector *result = InitSparseVector(a->nnz);
    uint16 *rx = SPARSEVEC_VALUES(result);

    float norm = sparsevector_norm(a);
    /* Return zero vector for zero norm */
    if (norm <= 0) {
        result->nnz = 0;
        SET_VARSIZE(result, SPARSEVEC_SIZE(0));
        PG_RETURN_SPARSEVECTOR_P(result);
    }

    uint32 offset = 0;
    for (uint32 i = 0; i < a->nnz; ++i) {
        result->indices[offset] = a->indices[i];
        /* no need to check overflow since ax[i] is gurenteed between [-1,1] */
        rx[offset] = float_to_half(half_to_float(ax[i]) / norm);
        if (unlikely(rx[offset] == 0)) {
            continue;
        }
        ++offset;
    }
    if (offset != result->nnz) {
        result->nnz = offset;
        SET_VARSIZE(result, SPARSEVEC_SIZE(offset));
    }
    PG_RETURN_SPARSEVECTOR_P(result);
}

Datum sparsevector_cosine_distance(PG_FUNCTION_ARGS)
{
    SparseVector *a = PG_GETARG_SPARSEVECTOR_P(0);
    SparseVector *b = PG_GETARG_SPARSEVECTOR_P(1);
    uint16 *ax = SPARSEVEC_VALUES(a);
    uint16 *bx = SPARSEVEC_VALUES(b);
    float norma = 0.0;
    float normb = 0.0;
    float similarity = SparsevecInnerProduct(a, b);
    for (uint32 i = 0; i < a->nnz; ++i) {
        float temp = half_to_float(ax[i]);
        norma += temp * temp;
    }
    for (uint32 i = 0; i < b->nnz; ++i) {
        float temp = half_to_float(bx[i]);
        normb += temp * temp;
    }
    similarity /= sqrtf(norma * normb);
    PG_RETURN_FLOAT4(1.0 - similarity);
}

Datum sparsevector_negative_inner_product(PG_FUNCTION_ARGS)
{
    SparseVector *a = PG_GETARG_SPARSEVECTOR_P(0);
    SparseVector *b = PG_GETARG_SPARSEVECTOR_P(1);
    PG_RETURN_FLOAT4(-SparsevecInnerProduct(a, b));
}
