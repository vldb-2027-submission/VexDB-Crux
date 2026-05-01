/**
 * Copyright ...
 * Definition of data contained by B+ tree.
 */

#ifndef DISKANN_CONTAINER_BPLUSTREE_DATA_IMPL_H
#define DISKANN_CONTAINER_BPLUSTREE_DATA_IMPL_H

#include "postgres.h"
#include "catalog/pg_collation.h"
#include "utils/date.h"
#include "access/itup.h"
#include "access/nbtree.h"
#include "access/hybridann/bplustree/base.h"

namespace disk_container {
struct BTTupleData : public IndexTupleData {
    size_t size() const
    {
        Assert(IndexTupleSize(this) > 0);
        return IndexTupleSize(this);
    }
    BlockNumber get_ptr() const { return BTreeInnerTupleGetDownLink(this); }
    void set_ptr(BlockNumber ptr) { BTreeInnerTupleSetDownLink(this, ptr); }
    static BTTupleData *create(const BTTupleData *x)
        { return (BTTupleData *)CopyIndexTuple((IndexTuple)x); }
    IndexTuple tuple() const { return (IndexTuple)this; }
};

inline int cmp_int1(Datum a, Datum b)
{
    return (int32)DatumGetInt8(a) - (int32)DatumGetInt8(b);
}
inline int cmp_int2(Datum a, Datum b)
{
    return (int32)DatumGetInt16(a) - (int32)DatumGetInt16(b);
}
inline int cmp_int4(Datum a, Datum b)
{
    int32 i1 = DatumGetInt32(a);
    int32 i2 = DatumGetInt32(b);
    return (i1 < i2) ? -1 : ((i1 > i2) ? 1 : 0);
}
inline int cmp_int8(Datum a, Datum b)
{
    int64 i1 = DatumGetInt64(a);
    int64 i2 = DatumGetInt64(b);
    return (i1 < i2) ? -1 : ((i1 > i2) ? 1 : 0);
}
inline int cmp_float4(Datum a, Datum b)
{
    float4 res = DatumGetFloat4(a) - DatumGetFloat4(b);
    return res > 0 ? 1 : (res < 0 ? -1 : 0);
}
inline int cmp_float8(Datum a, Datum b)
{
    float8 res = DatumGetFloat8(a) - DatumGetFloat8(b);
    return res > 0 ? 1 : (res < 0 ? -1 : 0);
}
inline int cmp_date(Datum a, Datum b)
{
    DateADT dateVal1 = DatumGetDateADT(a);
    DateADT dateVal2 = DatumGetDateADT(b);
    return (dateVal1 < dateVal2) ? -1 : ((dateVal1 > dateVal2) ? 1 : 0);
}
inline int cmp_timestamp(Datum a, Datum b)
{
#ifdef HAVE_INT64_TIMESTAMP
    Timestamp dt1 = DatumGetTimestamp(a);
    Timestamp dt2 = DatumGetTimestamp(b);
    return (dt1 < dt2) ? -1 : ((dt1 > dt2) ? 1 : 0);
#else
    return cmp_float8(a, b);
#endif
}

struct BTTupleScanKey : public BaseObject {
    typedef int (*cmp_func)(Datum a, Datum b);
    const TupleDesc tdesc;
    uint32 lower_inclusive{0};
    uint32 upper_inclusive{0};
    uint32 lower_nulls{1u};
    uint32 upper_nulls{1u};
    uint32 req_forw{0xfffeu};
    Datum *lower_bound;
    Datum *upper_bound;
    Oid *collations;
    FmgrInfo **proc;
    cmp_func *cmp_funcs;

    BTTupleScanKey(const BTTupleData &x, const TupleDesc tdesc,
                   Oid *in_collations, FmgrInfo **in_proc)
        : BTTupleScanKey(x, tdesc) { fill_info(in_collations, in_proc); }
    BTTupleScanKey(const TupleDesc tdesc, const Datum *lower_bound, const Datum *upper_bound,
                   const bool *lower_inclusive, const bool *upper_inclusive,
                   const bool *lower_nulls, const bool *upper_nulls, const bool *equals,
                   Oid *in_collations, FmgrInfo **in_proc)
        : BTTupleScanKey(lower_bound, upper_bound, lower_inclusive, upper_inclusive,
                         lower_nulls, upper_nulls, equals, tdesc)
    { fill_info(in_collations, in_proc); }
    BTTupleScanKey(BTTupleScanKey &&) = default;

    bool single_range() const
    {
        uint32 nattr = tdesc->natts;
        for (uint32 i = nattr - 1; i > 0; --i) {
            if (!n_req_forw(i) && (!datum_null<true>(i) || !datum_null<false>(i))) {
                return false;
            }
        }
        return true;
    }

    template <bool cmp_lower, bool range_check>
    int cmp(const IndexTuple tuple, uint32 &failed_attr) const
    {
        uint32 nattr = tdesc->natts;
        const auto set_failed_attr = [&failed_attr](uint32 i) {
            if (range_check) {
                failed_attr = i;
            }
        };
        for (uint32 i = 1; i < nattr; ++i) {
            if (datum_null<cmp_lower>(i)) {
                if (!range_check) {
                    return 0;
                }
                continue;
            }
            bool is_null;
            Datum v = index_getattr(tuple, i + 1u, tdesc, &is_null);
            if (is_null) {
                set_failed_attr(i);
                return range_check ? (cmp_lower ? -1 : 1) : -1;
            }
            Assert(proc[i]);
            Datum cmp_v = cmp_lower ? lower_bound[i] : upper_bound[i];
            int cmp_res = cmp_funcs[i] ?
                cmp_funcs[i](cmp_v, v) :
                DatumGetInt32(FunctionCall2Coll(proc[i], collations[i], cmp_v, v));
            if (range_check ? (cmp_lower ? cmp_res > 0 : cmp_res < 0) :
                              cmp_res != 0) {
                set_failed_attr(i);
                return cmp_res;
            }
            if ((!range_check || cmp_res == 0) && !inclusive<cmp_lower>(i)) {
                set_failed_attr(i);
                return cmp_lower ? 1 : -1;
            }
        }
        return 0;
    }
    bool operator<(const BTTupleData &x) const
    {
        uint32 unused;
        return cmp<true, false>(x.tuple(), unused) <= 0;
    }
    bool operator>(const BTTupleData &x) const
    {
        uint32 unused;
        return cmp<false, false>(x.tuple(), unused) >= 0;
    }
    bool operator()(const BTTupleData &x) const
    {
        uint32 unused;
        return cmp<true, true>(x.tuple(), unused) <= 0 && cmp<false, true>(x.tuple(), unused) >= 0;
    }
    /* forward is set only if returning false */
    bool operator()(const BTTupleData &x, bool &forward) const
    {
        uint32 failed_attr;
        int cmp_res = cmp<true, true>(x.tuple(), failed_attr);
        if (cmp_res > 0) {
            forward = !n_req_forw(failed_attr);
            return false;
        }
        cmp_res = cmp<false, true>(x.tuple(), failed_attr);
        if (cmp_res < 0) {
            forward = !n_req_forw(failed_attr);
            return false;
        }
        return true;
    }
    void destroy()
    {
        pfree_ext(lower_bound);
        pfree_ext(upper_bound);
        pfree_ext(collations);
        pfree_ext(proc);
    }
private:
    BTTupleScanKey(const TupleDesc tdesc) : tdesc(tdesc)
    {
        uint32 nattr = tdesc->natts;
        lower_bound = (Datum *)palloc(sizeof(Datum) * nattr);
        upper_bound = (Datum *)palloc(sizeof(Datum) * nattr);
        collations = (Oid *)palloc(sizeof(Oid) * nattr);
        proc = (FmgrInfo **)palloc(sizeof(FmgrInfo *) * nattr);
        cmp_funcs = (cmp_func *)palloc0(sizeof(cmp_func) * nattr);
    }
    BTTupleScanKey(const BTTupleData &x, const TupleDesc tdesc)
        : BTTupleScanKey(tdesc)
    {
        uint32 nattr = tdesc->natts;
        for (uint32 i = 1u; i < nattr; ++i) {
            bool is_null;
            Datum v = index_getattr(x.tuple(), i + 1u, tdesc, &is_null);
            if (is_null) {
                lower_nulls |= (1u << i);
            } else {
                lower_bound[i] = v;
            }
        }
        lower_inclusive = 0xFFu;
        upper_nulls = 0xFFu;
        upper_inclusive = 0;
    }
    BTTupleScanKey(const Datum *lower_bound, const Datum *upper_bound,
                   const bool *lower_inclusive, const bool *upper_inclusive,
                   const bool *lower_nulls, const bool *upper_nulls,
                   const bool *equals, const TupleDesc tdesc)
        : BTTupleScanKey(tdesc)
    {
        uint32 nattr = tdesc->natts;
        Assert(nattr <= 32);
        for (uint32 i = 1u; i < nattr; ++i) {
            this->lower_bound[i] = lower_bound[i];
        }
        for (uint32 i = 1u; i < nattr; ++i) {
            this->upper_bound[i] = upper_bound[i];
        }
        for (uint32 i = 1u; i < nattr; ++i) {
            if (lower_inclusive[i]) {
                this->lower_inclusive |= (1u << i);
            }
        }
        for (uint32 i = 1u; i < nattr; ++i) {
            if (upper_inclusive[i]) {
                this->upper_inclusive |= (1u << i);
            }
        }
        for (uint32 i = 1u; i < nattr; ++i) {
            if (lower_nulls[i]) {
                this->lower_nulls |= (1u << i);
            }
        }
        for (uint32 i = 1u; i < nattr; ++i) {
            if (upper_nulls[i]) {
                this->upper_nulls |= (1u << i);
            }
        }
        uint32 i = 1u;
        for (; i < nattr; ++i) {
            if (!equals[i]) {
                break;
            }
        }
        uint32 mask = 0xffffu << (i + 1u);
        req_forw = ~mask & 0xfffeu;
    }

    void fill_info(Oid *in_collations, FmgrInfo **in_proc)
    {
        uint32 nattr = tdesc->natts;
        collations = (Oid *)palloc(sizeof(Oid) * nattr);
        for (uint32 i = 0; i < nattr; ++i) {
            collations[i] = in_collations[i];
        }
        proc = (FmgrInfo **)palloc(sizeof(FmgrInfo *) * nattr);
        for (uint32 i = 0; i < nattr; ++i) {
            proc[i] = in_proc[i];
        }
        for (uint32 i = 0; i < nattr; ++i) {
            switch (tdesc->attrs[i].atttypid) {
                case INT1OID:
                    cmp_funcs[i] = cmp_int1;
                    break;
                case INT2OID:
                    cmp_funcs[i] = cmp_int2;
                    break;
                case INT4OID:
                    cmp_funcs[i] = cmp_int4;
                    break;
                case INT8OID:
                    cmp_funcs[i] = cmp_int8;
                    break;
                case FLOAT4OID:
                    cmp_funcs[i] = cmp_float4;
                    break;
                case FLOAT8OID:
                    cmp_funcs[i] = cmp_float8;
                    break;
                case DATEOID:
                    cmp_funcs[i] = cmp_date;
                    break;
                case TIMESTAMPOID:
                case TIMESTAMPTZOID:
                    cmp_funcs[i] = cmp_timestamp;
                    break;
                default:
                    break;
            }
        }
    }

    template <bool incl_lower>
    bool inclusive(uint32 attno) const
    {
        return incl_lower ? lower_inclusive & (1u << attno) : upper_inclusive & (1u << attno);
    }
    template <bool lower>
    bool datum_null(uint32 attno) const
    {
        return lower ? lower_nulls & (1u << attno) : upper_nulls & (1u << attno);
    }
    bool n_req_forw(uint32 attno) const { return req_forw & (1 << attno); }
};
} /* namespace disk_container */

#endif /* DISKANN_CONTAINER_BPLUSTREE_DATA_IMPL_H */
