#include "access/annvector/pq.h"
#include "access/annvector/distance/pq/pq_endecode.h"
#include "access/annvector/annkmeans.h"
#include "postgres.h"
#include "access/index_backend/taskpool.h"
#include "access/annvector/module/timer.h"

/*********************************************
 * PQ implementation
 *********************************************/

void ProductQuantizer::set_basic_values(size_t dim, size_t m , size_t nbits_) {
    d = dim;
    M = m;
    nbits = nbits_;

    set_derived_values();
}

void ProductQuantizer::set_fvec_L2sqr_ny_nearest_func()
{
   _fvec_L2sqr_ny_nearest_func = get_fvec_L2sqr_ny_nearest_func();
}

void ProductQuantizer::set_fvec_ny_distance_func(Metric metric)
{
    _fvec_ny_distance_func = get_fvec_ny_distance_func(metric);
}

void ProductQuantizer::set_dist_code_func()
{
    _distance_single_code_func = get_distance_single_code_func(nbits);
    _distance_four_codes_func = get_distance_four_codes_func(nbits);
}

void ProductQuantizer::free_resourses() {
    pfree(centroids);
}

void ProductQuantizer::set_derived_values() {
    // quite a few derived values
    if (d % M != 0) {
        ereport(ERROR,
            (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                errmsg("The dimension of the vector (%ld) should be a multiple of the number subquantizers (%ld)", d, M))); 
    }
    dsub = d / M;
    code_size = (nbits * M + 7) / 8;
    ksub = 1 << nbits;
    centroids = (float *)palloc_extended(d * ksub * sizeof(float), MCXT_ALLOC_ZERO | MCXT_ALLOC_HUGE);
}

void ProductQuantizer::set_params(FloatVectorArray subcenters, int m) {

    for (size_t i = 0; i < ksub; ++i) {
        float* vector = FloatVectorArrayGet(subcenters, i);
        errno_t eno = memcpy_s(get_centroids(m, i), dsub * sizeof(float), vector, dsub * sizeof(float));
        securec_check(eno, "\0", "\0");
    }
}

void ProductQuantizer::train(AnnKmeansState *kmeanstate, FloatVectorArray samples, int parallelWorkers, int maintenanceWorkMem)
{
    size_t n = samples->length;
    INIT_TASK_RUNNER();
    if (parallelWorkers > 0) {
        LAUNCH_CONSUMER(parallelWorkers);
    }
    START_TASK_POOL();

    int avgMaintenanceWorkMem = maintenanceWorkMem / std::min(parallelWorkers + 1, (int)M);
    ann_helper::Timer timer(M, 10, kmeanstate->indexName, kmeanstate->partIndexName);
    timer.set_stage("Kmeans train");
    const auto task = [this, n, samples, kmeanstate, avgMaintenanceWorkMem, &timer](size_t m) {
        FloatVectorArray subvectors = FloatVectorArrayInit(n, dsub);
        float* subvector = (float*)palloc0(sizeof(float) * dsub);
        FloatVectorArray subcenters = FloatVectorArrayInit(ksub, dsub);
        for (size_t j = 0; j < n; j++) {
            float* vector = FloatVectorArrayGet(samples, j);
            errno_t rc = memcpy_s((char*)(subvector), sizeof(float) * (dsub), &(vector[m * dsub]), sizeof(float)*(dsub));
            securec_check(rc, "\0", "\0");

            FloatVectorArraySet(subvectors, j, subvector);
            subvectors->length++;
        }
        AnnKmeans(kmeanstate, subvectors, subcenters, avgMaintenanceWorkMem, 0);
        set_params(subcenters, m);
        pfree(subvector);
        FloatVectorArrayFree(subcenters);
        FloatVectorArrayFree(subvectors);
        timer.report_loop("Kmeans train");
    };

    for (size_t m = 0; m < M; m++) {
        RUN_TASK(task, m);
    }

    WAIT_AND_END_TASK_POOL();
    timer.report("Kmeans train done");
    timer.destroy();
    DESTROY_TASK_RUNNER();

}

template <class PQEncoder>
void compute_code_generic(const ProductQuantizer& pq, const float* x, uint8_t* code) {
    float* distances = (float*)palloc(pq.ksub * sizeof(float));

    PQEncoder encoder(code, pq.nbits);
    for (size_t m = 0; m < pq.M; m++) {
        const float* xsub = x + m * pq.dsub;
        uint64_t idxm = 0;

        // the regular version
        idxm = pq._fvec_L2sqr_ny_nearest_func(
                distances,
                xsub,
                pq.get_centroids(m, 0),
                pq.dsub,
                pq.ksub);
        encoder.encode(idxm);
    }
    encoder.restore_code();
    pfree(distances);
}

void ProductQuantizer::compute_code(const float* x, uint8_t* code) const {
    switch (nbits) {
        case 8:
            compute_code_generic<PQEncoder8>(*this, x, code);
            break;

        case 16:
            compute_code_generic<PQEncoder16>(*this, x, code);
            break;

        default:
            compute_code_generic<PQEncoderGeneric>(*this, x, code);
            break;
    }
}

void ProductQuantizer::compute_distance_table(const float* x, float* dis_table) const {
    for (size_t m = 0; m < M; m++) {
        _fvec_ny_distance_func(dis_table + m * ksub,
                                x + m * dsub,
                                get_centroids(m, 0),
                                dsub,
                                ksub);
    }
}


float ProductQuantizer::distance_to_code(const uint8_t* code, const float *distTable) {
    return _distance_single_code_func(M, nbits, distTable, code);
}

void ProductQuantizer::distance_to_four_code(const float* distTable,
                            // codes
                            const uint8_t* code0,
                            const uint8_t* code1,
                            const uint8_t* code2,
                            const uint8_t* code3,
                            // computed distances
                            float& result0,
                            float& result1,
                            float& result2,
                            float& result3) {
    _distance_four_codes_func(M, nbits, distTable, code0, code1, code2, code3, result0, result1, result2, result3);
}