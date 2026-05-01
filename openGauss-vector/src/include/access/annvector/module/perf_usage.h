/**
 * Copyright ...
 * Tracking performance usage.
 */

#ifndef DISKANN_PERFUSAGE_H
#define DISKANN_PERFUSAGE_H

#define PERF_USAGE_LEVEL_NONE 0
#define PERF_USAGE_LEVEL_BASIC 1    /* count operation for each type */
#define PERF_USAGE_LEVEL_MEDIUM 2   /* count operation for each type and measure time of each operation */
#define PERF_USAGE_LEVEL_HIGH 3     /* reserved */
#ifndef PERF_USAGE
#define PERF_USAGE PERF_USAGE_LEVEL_NONE
#endif /* PERF_USAGE */
#if PERF_USAGE > PERF_USAGE_LEVEL_NONE
#if PERF_USAGE >= PERF_USAGE_LEVEL_MEDIUM
#include <chrono>
#endif /* PERF_USAGE >= PERF_USAGE_LEVEL_MEDIUM */
#include <vtl/vector>

#if PERF_USAGE >= PERF_USAGE_LEVEL_MEDIUM
#include "access/annvector/module/timer.h"
#endif /* PERF_USAGE >= PERF_USAGE_LEVEL_MEDIUM */
#include "c.h"
#include "utils/elog.h"

struct VecPerfUsage {
    size_t ncmp{0};
    size_t npqcmp{0};
    size_t nread{0};
    VecPerfUsage() = default;
};
struct PQPerfUsage {
    size_t ncmp{0};
    size_t nread{0};
    PQPerfUsage() = default;
};
struct RaBitQPerfUsage {
    size_t nbincmp{0};
    size_t nfullcmp{0};
    RaBitQPerfUsage() = default;
};
struct NeighborPerfUsage {
    size_t nread{0};
    size_t nwrite{0};
    NeighborPerfUsage() = default;
};
struct TagPerfUsage {
    size_t nread{0};
    size_t nwrite{0};
    TagPerfUsage() = default;
};
class PerfUsage {
public:
    enum PerfType : uint32 {
        VecCmp = 0, PQCmp, RaBitQBinCmp, RaBitQFullCmp, VecRead, PQRead, NeighborRead, NeighborWrite,
        TagRead, TagWrite, Prune, Ivf, Iter, Conflict, NumPerfTypes
    };
#if PERF_USAGE >= PERF_USAGE_LEVEL_MEDIUM
    Vector<uint64> start_time_stack;
    Vector<PerfType> perf_type_stack;
    uint64 perf_time[static_cast<size_t>(NumPerfTypes)]{{0}};
#endif /* PERF_USAGE >= PERF_USAGE_LEVEL_MEDIUM */
    VecPerfUsage vec;
    PQPerfUsage pq;
    RaBitQPerfUsage rabitq;
    NeighborPerfUsage neighbor;
    TagPerfUsage tag;
    size_t nprune{0};
    size_t nivf{0};
    size_t niter{0};
    size_t conflict{0};
    bool perf_reported{false};
    PerfUsage() = default;
#if PERF_USAGE >= PERF_USAGE_LEVEL_MEDIUM
    static inline uint64 get_time_ns() { return std::chrono::high_resolution_clock::now().time_since_epoch().count(); }
    void destroy()
    {
        ann_helper::optional_destroy(start_time_stack);
        ann_helper::optional_destroy(perf_type_stack);
    }
#endif /* PERF_USAGE >= PERF_USAGE_LEVEL_MEDIUM */
    void report()
    {
        if (perf_reported) {
            return;
        }
#if PERF_USAGE >= PERF_USAGE_LEVEL_MEDIUM
        char buf1[32], buf2[32];
        ann_helper::Timer::ns_to_str(perf_time[VecCmp], buf1);
        ann_helper::Timer::ns_to_str(perf_time[VecRead], buf2);
        ereport(NOTICE, (errmsg("VecPerfUsage: ncmp = %lu (%s), nread = %lu (%s)", vec.ncmp, buf1, vec.nread, buf2)));
        if (pq.ncmp > 0) {
            ann_helper::Timer::ns_to_str(perf_time[PQCmp], buf1);
            ann_helper::Timer::ns_to_str(perf_time[PQRead], buf2);
            ereport(NOTICE, (errmsg("PQPerfUsage: ncmp = %lu (%s), nread = %lu (%s)", pq.ncmp, buf1, pq.nread, buf2)));
        }
        if (rabitq.nbincmp > 0 || rabitq.nfullcmp > 0) {
            ann_helper::Timer::ns_to_str(perf_time[RaBitQBinCmp], buf1);
            ann_helper::Timer::ns_to_str(perf_time[RaBitQFullCmp], buf2);
            ereport(NOTICE, (errmsg("RaBitQPerfUsage: nbincmp = %lu (%s), nfullcmp = %lu (%s)", rabitq.nbincmp, buf1, rabitq.nfullcmp, buf2)));
        }
        ann_helper::Timer::ns_to_str(perf_time[NeighborRead], buf1);
        ann_helper::Timer::ns_to_str(perf_time[NeighborWrite], buf2);
        ereport(NOTICE, (errmsg("NeighborPerfUsage: nread = %lu (%s), nwrite = %lu (%s)", neighbor.nread, buf1, neighbor.nwrite, buf2)));
        ann_helper::Timer::ns_to_str(perf_time[TagRead], buf1);
        ann_helper::Timer::ns_to_str(perf_time[TagWrite], buf2);
        ereport(NOTICE, (errmsg("TagPerfUsage: nread = %lu (%s), nwrite = %lu (%s)", tag.nread, buf1, tag.nwrite, buf2)));
        if (nprune > 0) {
            ann_helper::Timer::ns_to_str(perf_time[Prune], buf1);
            ereport(NOTICE, (errmsg("nprune = %lu (%s)", nprune, buf1)));
        }
        if (nivf > 0) {
            ann_helper::Timer::ns_to_str(perf_time[Ivf], buf1);
            ereport(NOTICE, (errmsg("nivf = %lu (%s)", nivf, buf1)));
        }
        if (niter > 0) {
            ann_helper::Timer::ns_to_str(perf_time[Iter], buf1);
            ereport(NOTICE, (errmsg("niter = %lu (%s)", niter, buf1)));
        }
#else
        ereport(NOTICE, (errmsg("VecPerfUsage: ncmp = %lu, nread = %lu", vec.ncmp, vec.nread)));
        if (pq.ncmp > 0) {
            ereport(NOTICE, (errmsg("PQPerfUsage: ncmp = %lu, nread = %lu", pq.ncmp, pq.nread)));
        }
        ereport(NOTICE, (errmsg("NeighborPerfUsage: nread = %lu, nwrite = %lu", neighbor.nread, neighbor.nwrite)));
        ereport(NOTICE, (errmsg("TagPerfUsage: nread = %lu, nwrite = %lu", tag.nread, tag.nwrite)));
        if (nprune > 0) {
            ereport(NOTICE, (errmsg("nprune = %lu", nprune)));
        }
        if (nivf > 0) {
            ereport(NOTICE, (errmsg("nivf = %lu", nivf)));
        }
        if (niter > 0) {
            ereport(NOTICE, (errmsg("niter = %lu", niter)));
        }
#endif /* PERF_USAGE >= PERF_USAGE_LEVEL_MEDIUM */
        if (conflict > 0) {
            ereport(NOTICE, (errmsg("conflict = %lu", conflict)));
        }
        perf_reported = true;
    }
    void report_to(PerfUsage &other)
    {
        if (perf_reported) {
            return;
        }
#if PERF_USAGE >= PERF_USAGE_LEVEL_MEDIUM
        for (size_t i = 0; i < static_cast<size_t>(NumPerfTypes); ++i) {
            other.perf_time[i] += perf_time[i];
        }
#endif /* PERF_USAGE >= PERF_USAGE_LEVEL_MEDIUM */
        other.vec.ncmp += vec.ncmp;
        other.vec.nread += vec.nread;
        other.neighbor.nread += neighbor.nread;
        other.neighbor.nwrite += neighbor.nwrite;
        other.tag.nread += tag.nread;
        other.tag.nwrite += tag.nwrite;
        other.nprune += nprune;
        other.niter += niter;
        other.conflict += conflict;
        perf_reported = true;
    }
};
#if PERF_USAGE >= PERF_USAGE_LEVEL_MEDIUM
#define PerfRecordTimeType(perf, type) (perf).start_time_stack.push_back((perf).get_time_ns()); (perf).perf_type_stack.push_back(type)
#define PerfVecCmp(perf, x) do { (perf).vec.ncmp += (x); PerfRecordTimeType(perf, PerfUsage::VecCmp); } while (0)
#define PerfPQCmp(perf, x) do { (perf).pq.ncmp += (x); PerfRecordTimeType(perf, PerfUsage::PQCmp); } while (0)
#define PerfRaBitQBinCmp(perf) do { ++(perf).rabitq.nbincmp; PerfRecordTimeType(perf, PerfUsage::RaBitQBinCmp); } while (0)
#define PerfRaBitQFullCmp(perf) do { ++(perf).rabitq.nfullcmp; PerfRecordTimeType(perf, PerfUsage::RaBitQFullCmp); } while (0)
#define PerfVecRead(perf, x) do { (perf).vec.nread += (x); PerfRecordTimeType(perf, PerfUsage::VecRead); } while (0)
#define PerfVecReadCmp(perf, x) do { (perf).vec.nread += (x); PerfRecordTimeType(perf, PerfUsage::VecRead); } while (0)
#define PerfPQRead(perf, x) do { (perf).pq.nread += (x); PerfRecordTimeType(perf, PerfUsage::PQRead); } while (0)
#define PerfNeighborRead(perf, x) do { (perf).neighbor.nread += (x); PerfRecordTimeType(perf, PerfUsage::NeighborRead); } while (0)
#define PerfNeighborWrite(perf, x) do { (perf).neighbor.nwrite += (x); PerfRecordTimeType(perf, PerfUsage::NeighborWrite); } while (0)
#define PerfTagRead(perf, x) do { (perf).tag.nread += (x); PerfRecordTimeType(perf, PerfUsage::TagRead); } while (0)
#define PerfPrune(perf) do { ++(perf).nprune; PerfRecordTimeType(perf, PerfUsage::Prune); } while (0)
#define PerfIvf(perf) do { ++(perf).nivf; PerfRecordTimeType(perf, PerfUsage::Ivf); } while (0)
#define PerfIter(perf) do { ++(perf).niter; PerfRecordTimeType(perf, PerfUsage::Iter); } while (0)
#define PerfConflict(perf) ++(perf).conflict
#define PerfReport(perf) (perf).report()
#define PerfReportTo(perf, other) (perf).report_to(other)
#define PerfStop(perf) do {                                 \
    uint64 end_time = (perf).get_time_ns();               \
    Assert(!(perf).start_time_stack.empty());               \
    uint64 start_time = (perf).start_time_stack.back();   \
    (perf).start_time_stack.pop_back();                     \
    auto perf_type = (perf).perf_type_stack.back();         \
    (perf).perf_type_stack.pop_back();                      \
    (perf).perf_time[static_cast<size_t>(perf_type)] += end_time - start_time;  \
} while (0)
#else
#define PerfVecCmp(perf, x) (perf).vec.ncmp += (x)
#define PerfPQCmp(perf, x) (perf).pq.ncmp += (x)
#define PerfRaBitQBinCmp(perf) ++(perf).rabitq.nbincmp
#define PerfRaBitQFullCmp(perf) ++(perf).rabitq.nfullcmp
#define PerfVecRead(perf, x) (perf).vec.nread += (x)
#define PerfVecReadCmp(perf, x) do { (perf).vec.nread += (x); (perf).vec.ncmp += (x); } while (0)
#define PerfPQRead(perf, x) (perf).pq.nread += (x)
#define PerfNeighborRead(perf, x) (perf).neighbor.nread += (x)
#define PerfNeighborWrite(perf, x) (perf).neighbor.nwrite += (x)
#define PerfTagRead(perf, x) (perf).tag.nread += (x)
#define PerfPrune(perf) ++(perf).nprune
#define PerfIvf(perf) ++(perf).nivf
#define PerfIter(perf) ++(perf).niter
#define PerfConflict(perf) ++(perf).conflict
#define PerfReport(perf) (perf).report()
#define PerfReportTo(perf, other) (perf).report_to(other)
#define PerfStop(perf)
#endif /* PERF_USAGE >= PERF_USAGE_LEVEL_MEDIUM */
#else
class PerfUsage {};
#define PerfVecCmp(perf, x)
#define PerfPQCmp(perf, x)
#define PerfRaBitQBinCmp(perf)
#define PerfRaBitQFullCmp(perf)
#define PerfVecRead(perf, x)
#define PerfVecReadCmp(perf, x)
#define PerfPQRead(perf, x)
#define PerfNeighborRead(perf, x)
#define PerfNeighborWrite(perf, x)
#define PerfTagRead(perf, x)
#define PerfPrune(perf)
#define PerfIvf(perf)
#define PerfIter(perf)
#define PerfConflict(perf)
#define PerfReport(perf)
#define PerfReportTo(perf, other)
#define PerfStop(perf)
#endif /* PERF_USAGE */

#endif /* DISKANN_PERFUSAGE_H */
