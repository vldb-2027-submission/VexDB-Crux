#include "postgres.h"

#include <float.h>
#include <math.h>

#include "access/annvector/annkmeans.h"
#include "access/annvector/ivf.h"
#include "miscadmin.h"
#include "access/index_backend/taskpool.h"
#include "access/annvector/ann_utils.h"
#include "commands/vacuum.h"
#include "access/tableam.h"

using namespace ann_helper;

/*
 * Initialize with kmeans++
 *
 * https://theory.stanford.edu/~sergei/papers/kMeansPP-soda.pdf
 */
static void InitCenters(AnnKmeansState *kmeanstate, FloatVectorArray samples,
    FloatVectorArray centers, float *lowerBound, int parallelWorkers)
{
    distance_func procinfo;
    int i;
    int64 j;
    double sum;
    double choice;
    float *weight = (float *)palloc(samples->length * sizeof(float));
    int numCenters = centers->maxlen;
    int numSamples = samples->length;

    procinfo = kmeanstate->kmeansprocinfo;

    /* Choose an initial center uniformly at random */
    FloatVectorArraySet(centers, 0, FloatVectorArrayGet(samples, RandomInt() % samples->length));
    centers->length++;

    int totalParaWorkers = parallelWorkers + 1;
    double *parallelSum = (double *)palloc(totalParaWorkers * sizeof(double));

    const auto task = [samples, numCenters, centers, procinfo, weight, lowerBound, parallelSum](int batchIndex, int64 start, int64 end, int i) {
        for (int j = start; j < end; ++j) {
            float *vec = FloatVectorArrayGet(samples, j);

            /* Only need to compute distance for new center */
            /* TODO Use triangle inequality to reduce distance calculations */
            double distance = procinfo((vec), (FloatVectorArrayGet(centers, i)), centers->dim);

            /* Set lower bound */
            lowerBound[j * numCenters + i] = distance;

            /* Use distance squared for weighted probability distribution */
            distance *= distance;

            if (distance < weight[j]) {
                weight[j] = distance;
            }

            parallelSum[batchIndex] += weight[j];
        }
    };
    START_TASK_POOL();
    PARALLEL_BATCH_RUN_INIT();
    
    for (int64 j = 0; j < numSamples; ++j) {
        weight[j] = DBL_MAX;
    }

    for (i = 0; i < numCenters; i++) {
        CHECK_FOR_INTERRUPTS();
        
        sum = 0.0;
        for (int k = 0; k < totalParaWorkers; ++k) {
            parallelSum[k] = 0.0;
        }

        PARALLEL_BATCH_RUN_TASK_WAIT(numSamples, totalParaWorkers, task, i);

        for (int k = 0; k < totalParaWorkers; ++k) {
            sum += parallelSum[k];
        }

        /* Only compute lower bound on last iteration */
        if (i + 1 == numCenters) {
            break;
        }

        /* Choose new center using weighted probability distribution. */
        choice = sum * RandomDouble();
        for (j = 0; j < numSamples - 1; ++j) {
            choice -= weight[j];
            if (choice <= 0)
                break;
        }

        FloatVectorArraySet(centers, i + 1, FloatVectorArrayGet(samples, j));
        centers->length++;
    }

    WAIT_AND_END_TASK_POOL();
    pfree(weight);
    pfree(parallelSum);
}

/*
 * Apply norm to FloatVector
 */
static void ApplyNorm(distance_func normprocinfo, float * vec, int dim)
{
    double norm = normprocinfo(vec, vec, dim);
    /* TODO Handle zero norm */
    if (norm > 0) {
        for (int i = 0; i < dim; i++) {
            vec[i] /= norm;
        }
    }
}

/*
 * Compare vectors
 */
static int CompareVectors(const void *a, const void *b, void* arg)
{
    int dim = *((int*)arg);
    float *fa = (float*)a;
    float *fb = (float*)b;
    for (int i = 0; i < dim; i++) {
        if (fa[i] < fb[i]) {
            return -1;
        }

        if (fa[i] > fb[i]) {
            return 1;
        }
    }

    return 0;
}

/*
 * Quick approach if we have little data
 */
static void QuickCenters(AnnKmeansState *kmeanstate, FloatVectorArray samples, FloatVectorArray centers)
{
    int i;
    int j;
    float *vec;
    int dimensions = centers->dim;
    int dim = centers->dim;

    /* Copy existing vectors while avoiding duplicates */
    if (samples->length > 0) {
        qsort_arg(samples->items, samples->length, (sizeof(float) * samples->dim), CompareVectors, (void*)&dim);
        for (i = 0; i < samples->length; i++) {
            vec = FloatVectorArrayGet(samples, i);
            if (i == 0 || CompareVectors(vec, FloatVectorArrayGet(samples, i - 1), (void*)&dim) != 0) {
                FloatVectorArraySet(centers, centers->length, vec);
                centers->length++;
            }
        }
    }

    /* Fill remaining with random data */
    while (centers->length < centers->maxlen) {
        vec = FloatVectorArrayGet(centers, centers->length);
        for (j = 0; j < dimensions; ++j) {
            vec[j] = RandomDouble();
        }

        /* Normalize if needed (only needed for random centers) */
        if (kmeanstate->kmeansnormprocinfo != NULL) {
            ApplyNorm(kmeanstate->kmeansnormprocinfo, vec, dimensions);
        }

        centers->length++;
    }
}

/*
 * Use Elkan for performance. This requires distance function to satisfy triangle inequality.
 *
 * We use L2 distance for L2 (not L2 squared like index scan)
 * and angular distance for inner product and cosine distance
 *
 * https://www.aaai.org/Papers/ICML/2003/ICML03-022.pdf
 */
static void ElkanKmeans(AnnKmeansState *kmeanstate, FloatVectorArray samples,
    FloatVectorArray centers, int avgMaintenanceWorkMem, int parallelWorkers)
{
    distance_func procinfo;
    distance_func normprocinfo;
    float *vec;
    float *newCenter;
    int iteration;
    int64 j;
    int64 k;
    int dimensions = centers->dim;
    int numCenters = centers->maxlen;
    int numSamples = samples->length;
    FloatVectorArray newCenters;
    int *centerCounts;
    int *closestCenters;
    float *lowerBound;
    float *upperBound;
    float *s;
    float *halfcdist;
    float *newcdist;
    bool changes;
    int closestCenter;
    bool rjreset;

    /* Calculate allocation sizes */
    Size samplesSize = FLOATVECTOR_ARRAY_SIZE(samples->maxlen, samples->dim);
    Size centersSize = FLOATVECTOR_ARRAY_SIZE(centers->maxlen, centers->dim);
    Size newCentersSize = FLOATVECTOR_ARRAY_SIZE(numCenters, dimensions);
    Size centerCountsSize = sizeof(int) * numCenters;
    Size closestCentersSize = sizeof(int) * numSamples;
    Size lowerBoundSize = sizeof(float) * numSamples * numCenters;
    Size upperBoundSize = sizeof(float) * numSamples;
    Size sSize = sizeof(float) * numCenters;
    Size halfcdistSize = sizeof(float) * numCenters * numCenters;
    Size newcdistSize = sizeof(float) * numCenters;

    /* Calculate total size */
    Size totalSize = samplesSize + centersSize + newCentersSize + centerCountsSize +
        closestCentersSize + lowerBoundSize + upperBoundSize + sSize + halfcdistSize + newcdistSize;

    /* Check memory requirements */
    /* Add one to error message to ceil */
    if (totalSize > (Size) avgMaintenanceWorkMem * 1024L)
        ereport(ERROR,
                (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                 errmsg("memory required is %zu MB, average maintenance work memory is %d MB",
                        totalSize / (1024 * 1024) + 1, avgMaintenanceWorkMem / 1024)));

    /* Ensure indexing does not overflow */
    if (numCenters * numCenters > INT_MAX)
        elog(ERROR, "Indexing overflow detected. Please report a bug.");

    /* Set support functions */
    procinfo = kmeanstate->kmeansprocinfo;
    normprocinfo = kmeanstate->kmeansnormprocinfo;

    /* Allocate space */
    /* Use float instead of double to save memory */
    centerCounts = (int *)palloc(centerCountsSize);
    closestCenters = (int *)palloc(closestCentersSize);
    lowerBound = (float *)palloc_huge(CurrentMemoryContext,lowerBoundSize);
    upperBound = (float *)palloc(upperBoundSize);
    s = (float *)palloc(sSize);
    halfcdist = (float *)palloc_huge(CurrentMemoryContext, halfcdistSize);
    newcdist = (float *)palloc(newcdistSize);

    newCenters = FloatVectorArrayInit(numCenters, dimensions);

    START_TASK_POOL();

    int totalParaWorkers = parallelWorkers + 1;
    PARALLEL_BATCH_RUN_INIT();

    /* Pick initial centers */
    IvfBench("ElkanKmeans InitCenters", InitCenters(kmeanstate, samples, centers, lowerBound, parallelWorkers));

    const auto findClosestCenterTask = [lowerBound, upperBound, closestCenters, numCenters](int batchIndex, int start, int end) {
        for (int64 j = start; j < end; ++j)
        {
            double minDistance = DBL_MAX;
            int closestCenter = 0;

            /* Find closest center */
            for (int64 k = 0; k < numCenters; k++)
            {
                /* TODO Use Lemma 1 in k-means++ initialization */
                double distance = lowerBound[j * numCenters + k];

                if (distance < minDistance)
                {
                    minDistance = distance;
                    closestCenter = k;
                }
            }

            upperBound[j] = minDistance;
            closestCenters[j] = closestCenter;
        }
    };

    /* Assign each x to its closest initial center c(x) = argmin d(x,c) */
    PARALLEL_BATCH_RUN_TASK_WAIT(numSamples, totalParaWorkers, findClosestCenterTask);

    const auto step1aTask = [centers, numCenters, halfcdist, procinfo](int batchIndex, int start, int end) {
        for (int64 j = start; j < end; ++j) {
            float *vec = FloatVectorArrayGet(centers, j);
            for (int64 k = j + 1; k < numCenters; k++) {
                double distance = 0.5 * procinfo(vec, FloatVectorArrayGet(centers, k), centers->dim);
                halfcdist[j * numCenters + k] = distance;
                halfcdist[k * numCenters + j] = distance;
            }
        }
    };

    const auto step1bTask = [numCenters, halfcdist, s](int batchIndex, int start, int end) {
        for (int64 j = start; j < end; ++j) {
            double minDistance = DBL_MAX;

            for (int64 k = 0; k < numCenters; k++) {
                if (j == k) {
                    continue;
                }

                double distance = halfcdist[j * numCenters + k];
                if (distance < minDistance) {
                    minDistance = distance;
                }
            }

            s[j] = minDistance;
        }
    };

    const auto step2_3Task = [&](int batchIndex, int start, int end, bool rjreset) {
        double dxcx;
        double dxc;
        for (int64 j = start; j < end; ++j) {
            /* Step 2: Identify all points x such that u(x) <= s(c(x)) */
            if (upperBound[j] <= s[closestCenters[j]]) {
                continue;
            }

            bool rj = rjreset;

            for (int64 k = 0; k < numCenters; k++) {
                /* Step 3: For all remaining points x and centers c */
                if (k == closestCenters[j]) {
                    continue;
                }

                if (upperBound[j] <= lowerBound[j * numCenters + k]) {
                    continue;
                }

                if (upperBound[j] <= halfcdist[closestCenters[j] * numCenters + k]) {
                    continue;
                }

                float *vec = FloatVectorArrayGet(samples, j);

                /* Step 3a */
                if (rj) {
                    dxcx = procinfo(vec, FloatVectorArrayGet(centers, closestCenters[j]), centers->dim);

                    /* d(x,c(x)) computed, which is a form of d(x,c) */
                    lowerBound[j * numCenters + closestCenters[j]] = dxcx;
                    upperBound[j] = dxcx;

                    rj = false;
                } else {
                    dxcx = upperBound[j];
                }

                /* Step 3b */
                if (dxcx > lowerBound[j * numCenters + k] || dxcx > halfcdist[closestCenters[j] * numCenters + k]) {
                    dxc = procinfo((vec), (FloatVectorArrayGet(centers, k)), centers->dim);

                    /* d(x,c) calculated */
                    lowerBound[j * numCenters + k] = dxc;

                    if (dxc < dxcx) {
                        closestCenters[j] = k;

                        /* c(x) changed */
                        upperBound[j] = dxc;

                        changes = true;
                    }
                }
            }
        }
    };

    const auto step4aTask = [newCenters, dimensions, centerCounts](int batchIndex, int64 start, int64 end){
        for (int64 j = start; j < end; ++j) {
            float *vec = FloatVectorArrayGet(newCenters, j);
            for (int64 k = 0; k < dimensions; k++) {
                vec[k] = 0.0;
            }
            centerCounts[j] = 0;
        }
    };

    const auto step4cTask = [&](int batchIndex, int64 start, int64 end) {
        for (int64 j = start; j < end; ++j) {
            float *vec = FloatVectorArrayGet(newCenters, j);

            if (centerCounts[j] > 0) {
                /* Double avoids overflow, but requires more memory */
                /* TODO Update bounds */
                for (int64 k = 0; k < dimensions; k++) {
                    if (isinf(vec[k])) {
                        vec[k] = vec[k] > 0 ? FLT_MAX : -FLT_MAX;
                    }
                }

                for (int64 k = 0; k < dimensions; k++) {
                    vec[k] /= centerCounts[j];
                }
            } else {
                /* TODO Handle empty centers properly */
                for (int64 k = 0; k < dimensions; k++) {
                    vec[k] = RandomDouble();
                }
            }

            /* Normalize if needed */
            if (normprocinfo != NULL) {
                ApplyNorm(normprocinfo, vec, dimensions);
            }
        }
    };


    const auto step5aTask = [newcdist, centers, newCenters, procinfo](int batchIndex, int64 start, int64 end) {
        for (int64 j = start; j < end; ++j) {
            newcdist[j] = procinfo((FloatVectorArrayGet(centers, j)), (FloatVectorArrayGet(newCenters, j)), centers->dim);
        }
    };

    const auto step5bTask = [numCenters, lowerBound, newcdist](int batchIndex, int64 start, int64 end) {
        for (int64 j = start; j < end; ++j) {
            for (int64 k = 0; k < numCenters; k++) {
                double distance = lowerBound[j * numCenters + k] - newcdist[k];

                if (distance < 0) {
                    distance = 0;
                }

                lowerBound[j * numCenters + k] = distance;
            }
        }
    };

    const auto step6Task = [upperBound, newcdist, closestCenters](int batchIndex, int64 start, int64 end) {
        for (int64 j = start ; j < end; ++j) {
            upperBound[j] += newcdist[closestCenters[j]];
        }
    };

    const auto step7Task = [centers, newCenters, dimensions](int batchIndex, int64 start, int64 end) {
        for (int64 j = start; j < end; ++j) {
            errno_t rc = memcpy_s(FloatVectorArrayGet(centers, j), FLOATVECTOR_COMPACT_SIZE(dimensions), FloatVectorArrayGet(newCenters, j), FLOATVECTOR_COMPACT_SIZE(dimensions));
            securec_check(rc, "\0", "\0");
        }
    };

    /* Give 500 iterations to converge */
    for (iteration = 0; iteration < 500; iteration++) {
        /* Can take a while, so ensure we can interrupt */
        CHECK_FOR_INTERRUPTS();

        changes = false;

        /* Step 1: For all centers, compute distance */
        /* Step 1a */
        PARALLEL_BATCH_RUN_TASK_WAIT(numCenters, totalParaWorkers, step1aTask);

        /* Step 1b */
        /* For all centers c, compute s(c) */
        PARALLEL_BATCH_RUN_TASK_WAIT(numCenters, totalParaWorkers, step1bTask);

        rjreset = iteration != 0;

        PARALLEL_BATCH_RUN_TASK_WAIT(numSamples, totalParaWorkers, step2_3Task, rjreset);

        /* Step 4: For each center c, let m(c) be mean of all points assigned */
        /* Step 4a */
        PARALLEL_BATCH_RUN_TASK_WAIT(numCenters, totalParaWorkers, step4aTask);

        /* Step 4b */
        for (j = 0; j < numSamples; ++j) {
            vec = FloatVectorArrayGet(samples, j);
            closestCenter = closestCenters[j];

            /* Increment sum and count of closest center */
            newCenter = FloatVectorArrayGet(newCenters, closestCenter);
            for (k = 0; k < dimensions; k++)
                newCenter[k] += vec[k];

            centerCounts[closestCenter] += 1;
        }

        /* Step 4c */
        PARALLEL_BATCH_RUN_TASK_WAIT(numCenters, totalParaWorkers, step4cTask);

        /* Step 5a */
        PARALLEL_BATCH_RUN_TASK_WAIT(numCenters, totalParaWorkers, step5aTask);

        /* Step 5b */
        PARALLEL_BATCH_RUN_TASK_WAIT(numSamples, totalParaWorkers, step5bTask);

        /* Step 6 */
        /* We reset r(x) before Step 3 in the next iteration */
        PARALLEL_BATCH_RUN_TASK_WAIT(numSamples, totalParaWorkers, step6Task);

        /* Step 7 */
        PARALLEL_BATCH_RUN_TASK_WAIT(numCenters, totalParaWorkers, step7Task);

        if (!changes && iteration != 0) {
            break;
        }
    }

    FloatVectorArrayFree(newCenters);
    pfree(centerCounts);
    pfree(closestCenters);
    pfree(lowerBound);
    pfree(upperBound);
    pfree(s);
    pfree(halfcdist);
    pfree(newcdist);
    END_TASK_POOL();
}

/*
 * Detect issues with centers
 */
static void CheckCenters(AnnKmeansState *kmeanstate, FloatVectorArray centers)
{
    float *vec;
    int i;
    int j;
    double norm;
    int dim = centers->dim;

    if (centers->length != centers->maxlen) {
        elog(ERROR, "Not enough centers. Please report a bug.");
    }

    /* Ensure no NaN or infinite values */
    for (i = 0; i < centers->length; i++) {
        vec = FloatVectorArrayGet(centers, i);
        for (j = 0; j < dim; ++j) {
            if (isnan(vec[j])) {
                elog(ERROR, "NaN detected. Please report a bug.");
            }

            if (isinf(vec[j])) {
                elog(ERROR, "Infinite value detected. Please report a bug.");
            }
        }
    }

    /* Ensure no duplicate centers */
    /* Fine to sort in-place */
    qsort_arg(centers->items, centers->length, (sizeof(float) * centers->dim), CompareVectors, (void*)&dim);
    for (i = 1; i < centers->length; i++) {
        if (CompareVectors(FloatVectorArrayGet(centers, i), FloatVectorArrayGet(centers, i - 1), (void*)&dim) == 0 &&
            !kmeanstate->skipCheckDuplicate) {
            elog(ERROR, "Duplicate centers detected. Please report a bug.");
        }
    }

    /* Ensure no zero vectors for cosine distance */
    /* Check NORM_PROC instead of KMEANS_NORM_PROC */
    if (kmeanstate->metric == Metric::COSINE || kmeanstate->metric == Metric::FAST_COSINE) {
        distance_func norm_dist_func = get_general_distance_func(Metric::L2_NORM, dim);
        for (i = 0; i < centers->length; i++) {
            norm = norm_dist_func(FloatVectorArrayGet(centers, i), FloatVectorArrayGet(centers, i), centers->dim);
            if (norm == 0) {
                elog(ERROR, "Zero norm detected. For PQ, please specify parameter by_residual "
                            "to false and retry, other case please report a bug.");
            }
        }
    }
}

/*
 * Perform naive k-means centering
 * We use spherical k-means for inner product and cosine
 */
void AnnKmeans(AnnKmeansState *kmeanstate, FloatVectorArray samples, FloatVectorArray centers,
    int avgMaintenanceWorkMem, int parallelWorkers)
{
    if (samples->length <= centers->maxlen) {
        QuickCenters(kmeanstate, samples, centers);
    } else {
        ElkanKmeans(kmeanstate, samples, centers, avgMaintenanceWorkMem, parallelWorkers);
    }

    IvfBench("AnnKmeans CheckCenters", CheckCenters(kmeanstate, centers));
}

void setupKmeansState(Metric metric, Relation index, AnnKmeansState *kmeanstate, int dimension,
    bool ispq,  bool pqtrain)
{
    if (ispq) {
        if (metric == Metric::L2 || metric == Metric::COSINE || metric == Metric::FAST_COSINE) {
            kmeanstate->kmeansprocinfo = get_general_distance_func(Metric::L2_SQRT, dimension);
            /*pq cosine use equalent l2 after normalize, so in kmeans don't do nomalize*/
            kmeanstate->kmeansnormprocinfo = NULL;
        } else if (metric == Metric::INNER_PRODUCT) {
            if (pqtrain) {
                kmeanstate->kmeansprocinfo = get_general_distance_func(Metric::L2_SQRT, dimension);
                kmeanstate->kmeansnormprocinfo = NULL;
            } else {
                kmeanstate->kmeansprocinfo = get_general_distance_func(Metric::SPHERICAL, dimension);
                kmeanstate->kmeansnormprocinfo = get_general_distance_func(Metric::L2_NORM, dimension);
            }
        } else {
            elog(ERROR, "Distance Metric type(%d) is not handled during setup kemans state", metric);
        }
    } else {
        if (metric == Metric::L2) {
            kmeanstate->kmeansprocinfo = get_general_distance_func(Metric::L2_SQRT, dimension);
            kmeanstate->kmeansnormprocinfo = NULL;
        } else if (metric == Metric::COSINE || metric == Metric::FAST_COSINE 
                    || metric == Metric::INNER_PRODUCT) {
            kmeanstate->kmeansprocinfo = get_general_distance_func(Metric::SPHERICAL, dimension);
            kmeanstate->kmeansnormprocinfo = get_general_distance_func(Metric::L2_NORM, dimension);
        } else {
            elog(ERROR, "Distance Metric type(%d) is not handled during setup kemans state", metric);
        }
    }

    kmeanstate->skipCheckDuplicate = pqtrain;
    kmeanstate->metric = metric;
    populate_index_partition_name(index, kmeanstate->indexName, kmeanstate->partIndexName);
}

int GetSampleNumbers(Relation heap, Relation index, int listNum)
{
    /* Skip samples for unlogged table */
    if (heap == NULL && !isHybridIndex(index)) {
        return 1;
    }

    /* The number of samples has a large effect on index build time */
    int numSamples = listNum * 50;
    if (numSamples < 10000) {
        numSamples = 10000;
    }

    if(numSamples > MAX_SAMPLE_VECTOR_NUM){
        numSamples = MAX_SAMPLE_VECTOR_NUM;
    }

    return numSamples;
}

static void add_sample_internal(SampleState *sampleState, Datum *values)
{
    FloatVectorArray samples = sampleState->samples;
    uint32 target_sample_num = sampleState->workerInfo.sampleNum;

    Pointer vec_p = NULL;
    const DistPrecisionType precision_type = sampleState->precision_type;
    const uint32 dim = sampleState->dim;
    char *v = DatumGetVector(values[0], precision_type, &vec_p);

    char *value = v;
    /*
     * Normalize with KMEANS_NORM_PROC since spherical distance function
     * expects unit vectors
     */
    if (sampleState->need_norm) {
        char *temp = alloc_vector(dim * VEC_ELEM_SIZE(precision_type));
        auto func = get_vector_preprocess_func(Metric::FAST_COSINE, precision_type);
        func(value, dim, temp);
        value = temp;
    }

    float* half2float = NULL;
    if (precision_type == DistPrecisionType::HALF) {
        half2float = alloc_floatvector(dim);
        halfs_to_floats((half *)value, half2float, dim);
        if (value != v) {
            free_vector(value);
        }
        value = (char *)half2float;
    }

    if (sampleState->workerInfo.sampleCount < target_sample_num) {
        FloatVectorArraySet(samples, sampleState->workerInfo.startIndex + sampleState->workerInfo.sampleCount, value);
        sampleState->fillflags[sampleState->workerInfo.startIndex + sampleState->workerInfo.sampleCount] = true;
        sampleState->workerInfo.sampleCount++;
    } else {
        if (sampleState->workerInfo.rowstoskip < 0) {
            sampleState->workerInfo.rowstoskip =
                anl_get_next_S(sampleState->workerInfo.sampleCount, target_sample_num, &(sampleState->workerInfo.rstate));
        }

        if (sampleState->workerInfo.rowstoskip <= 0) {
            uint32 offset = static_cast<uint32>(target_sample_num * anl_random_fract());
            Assert(offset < target_sample_num);
            FloatVectorArraySet(samples, sampleState->workerInfo.startIndex + offset, value);
            sampleState->fillflags[sampleState->workerInfo.startIndex + offset] = true;
        }

        sampleState->workerInfo.rowstoskip -= 1;
    }

    if (value != v) {
        free_vector(value);
    }

    if (vec_p != DatumGetPointer(values[0])) {
        pfree(vec_p);
    }
}

/* just keep compiler silent */
#define UNUSED_ARG(_arg_) ((void)(_arg_))

static void sample_callback(Relation index, HeapTuple hup, Datum *values,
    const bool *is_null, bool is_tuple_alive, void *state)
{
    UNUSED_ARG(index);
    UNUSED_ARG(hup);
    UNUSED_ARG(is_tuple_alive);
    SampleState *sampleState = (SampleState *)state;
    if (!is_null[0]) {
		add_sample_internal(sampleState, values);
    }
}

static void initParallelSampleWorkerInfo(SampleState *sampleState, int batchIndex,
    int totalParallelWorkerNum, int sample_target_num, Relation heap, Relation index, bool need_norm,
    bool *fillflags, int dimensions, FloatVectorArray samples, DistPrecisionType precision_type)
{
    sampleState->samples = samples;
    sampleState->need_norm = need_norm;
    sampleState->dim = (uint32)dimensions;
    sampleState->precision_type = precision_type;
    sampleState->fillflags = fillflags;
    sampleState->workerInfo.batchIndex = batchIndex;
    sampleState->workerInfo.sampleCount = 0;
    sampleState->workerInfo.totalParallelWorkerNum = totalParallelWorkerNum;

    int avgNum = sample_target_num / totalParallelWorkerNum;                   
    int remainingNum = sample_target_num % totalParallelWorkerNum;
    sampleState->workerInfo.sampleNum = avgNum;
    sampleState->workerInfo.startIndex = batchIndex * avgNum;

    if (batchIndex == 0) {
        sampleState->workerInfo.sampleNum += remainingNum;
    } else {
        sampleState->workerInfo.startIndex += remainingNum;
    }
    sampleState->workerInfo.rowstoskip = -1;
    sampleState->workerInfo.rstate = anl_init_selection_state(sampleState->workerInfo.sampleNum);

    if (IsBgWorkerProcess()) {
        Oid heaprelid;
        Oid indexrelid;
        Oid heappartid;
        Oid indexpartid;
        Relation heapRel;
        Relation indexRel;
        Relation targetheap;
        Relation targetindex;
        Partition heappart = NULL;
        Partition indexpart = NULL;
        if (RelationIsPartition(heap)) {
            heaprelid = GetBaseRelOidOfParition(heap);
            indexrelid = GetBaseRelOidOfParition(index);
            heappartid = RelationGetRelid(heap);
            indexpartid = RelationGetRelid(index);
        } else {
            heaprelid = RelationGetRelid(heap);
            indexrelid = RelationGetRelid(index);
            heappartid = InvalidOid;
            indexpartid = InvalidOid;
        }

        /* Open relations within worker */
        heapRel = heap_open(heaprelid, NoLock);
        indexRel = index_open(indexrelid, NoLock);
        if (RelationIsPartition(heap)) {
            sampleState->workerInfo.parentHeapRel = heapRel;
            sampleState->workerInfo.parentIndexRel = indexRel;
            heappart = partitionOpen(heapRel, heappartid,  NoLock);
            indexpart = partitionOpen(indexRel, indexpartid, NoLock);
            targetheap = partitionGetRelation(heapRel, heappart);
            targetindex = partitionGetRelation(indexRel, indexpart);
        } else {
            targetheap = heapRel;
            targetindex = indexRel;
        }
        sampleState->workerInfo.heapRel = targetheap;
        sampleState->workerInfo.indexRel = targetindex;
        sampleState->workerInfo.heappart = heappart;
        sampleState->workerInfo.indexpart = indexpart;
    } else  {
        sampleState->workerInfo.heapRel = heap;
        sampleState->workerInfo.indexRel = index;
        sampleState->workerInfo.heappart = NULL;
        sampleState->workerInfo.indexpart = NULL;
    }
}

static void cleanupParallelSampleWorkerInfo(SampleState *sampleState)
{
    if (!IsBgWorkerProcess()) {
        return;
    }

    /* Close relations within worker */
    if (sampleState->workerInfo.heappart) {
        releaseDummyRelation(&sampleState->workerInfo.indexRel);
        releaseDummyRelation(&sampleState->workerInfo.heapRel);
        partitionClose(sampleState->workerInfo.parentIndexRel, sampleState->workerInfo.indexpart, NoLock);
        partitionClose(sampleState->workerInfo.parentHeapRel, sampleState->workerInfo.heappart, NoLock);
        index_close(sampleState->workerInfo.parentIndexRel, NoLock);
        heap_close(sampleState->workerInfo.parentHeapRel, NoLock);
    } else {
        index_close(sampleState->workerInfo.indexRel, NoLock);
        heap_close(sampleState->workerInfo.heapRel, NoLock);
    }
}

/*
 * Sample rows with same logic as ANALYZE
 */
void ann_sample_rows(FloatVectorArray samples, Relation heap, Relation index, int dimensions,
    int parallelWorkers, bool need_norm, DistPrecisionType precision_type)
{
    int sample_target_num = samples->maxlen;
    BlockNumber total_blocks = RelationGetNumberOfBlocks(heap);
    MemoryContext tmpCtx = AllocSetContextCreate(CurrentMemoryContext,
        "Sample temporary context", ALLOCSET_DEFAULT_SIZES);

    MemoryContext old_ctx = MemoryContextSwitchTo(tmpCtx);
    BlockSamplerData bs;
    BlockSampler_Init(&bs, total_blocks, sample_target_num);
    BlockNumber sampleBlockNumbers[MAX_SAMPLE_VECTOR_NUM];
    int number = 0;
    int totalParallelWorkers = parallelWorkers + 1;
    slock_t mutex;
    SpinLockInit(&mutex);
    bool *fillflags = (bool *)palloc0(samples->maxlen);

    while (BlockSampler_HasMore(&bs)) {
        BlockNumber targblock = BlockSampler_Next(&bs);
        sampleBlockNumbers[number++] = targblock;
    }

    const auto task = [&](int batchIndex, int start, int end) {
        SampleState *sampleState = (SampleState*)palloc(sizeof(SampleState));
        initParallelSampleWorkerInfo(sampleState, batchIndex, totalParallelWorkers,
            sample_target_num, heap, index, need_norm, fillflags, dimensions, samples, precision_type);
        IndexInfo *indexInfo = BuildIndexInfo(sampleState->workerInfo.indexRel);
        for (int i = start; i < end; ++i) {
            tableam_index_build_range_scan(sampleState->workerInfo.heapRel,
                sampleState->workerInfo.indexRel, indexInfo, false, true, false,
                sampleBlockNumbers[i], 1, sample_callback, (void *)sampleState, NULL);
        }
        pfree(indexInfo);

        SpinLockAcquire(&mutex);
        samples->length += sampleState->workerInfo.sampleCount;
        SpinLockRelease(&mutex);
        cleanupParallelSampleWorkerInfo(sampleState);
        pfree(sampleState);
    };

    INIT_TASK_RUNNER();
    if (parallelWorkers > 0) {
        LAUNCH_CONSUMER(parallelWorkers);
    }
    START_TASK_POOL();
    PARALLEL_BATCH_RUN_INIT();
    PARALLEL_BATCH_RUN_TASK_WAIT(number, totalParallelWorkers, task)
    END_TASK_POOL();
    DESTROY_TASK_RUNNER();

    if (parallelWorkers > 0 && samples->length < samples->maxlen) {
        /* there are some zero dim vectors(palloc initialized) in [0,length), overwrite by sampled vectors from [length, maxlen) */
        int left = samples->length;
        int right = samples->maxlen - 1;
        for (int i = 0; i < left; ++i) {
            if (fillflags[i]) {
                continue;
            }
            for (int j = right; j >= left; --j) {
                float *vectemp = FloatVectorArrayGet(samples, j);
                if (fillflags[j]) {
                    FloatVectorArraySet(samples, i , vectemp);
                    right = j - 1;
                    break;
                }
            }
        }
    }

    pfree(fillflags);
    MemoryContextSwitchTo(old_ctx);
    MemoryContextReset(tmpCtx);
}
