#include "postgres.h"
#include "access/annvector/quantizer.h"

void validate_quantizer(const char *value) {
    if (!value) {
        return;
    }
    if (strcmp(value, "none") != 0 &&
        strcmp(value, "pq") != 0 &&
        strcmp(value, "rabitq") != 0) {
        ereport(ERROR,
            (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
             errmsg("invalid quantizer type \"%s\"", value)));
    }
}

QuantizerType extract_qt(const char *value) {
    if (strcmp(value, "none") == 0) {
        return QuantizerType::NONE;
    }
    if (strcmp(value, "pq") == 0) {
        return QuantizerType::PQ;
    }
    if (strcmp(value, "rabitq") == 0) {
        return QuantizerType::RABITQ;
    }
    __builtin_unreachable();
}