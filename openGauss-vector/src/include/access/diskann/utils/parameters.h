/**
 * Copyright ...
 * Parameter utilities.
 */

#ifndef DISKANN_UTILS_PARAMETERS_H
#define DISKANN_UTILS_PARAMETERS_H

#include <vtl/vector>
#include "utils/elog.h"
#include "utils/palloc.h"

namespace ann_helper {
enum StringParamStatus : uint8 {
    SUCCESS, INVALID_PARAM, OUT_OF_RANGE, OUT_OF_MAX_SIZE
};
class StringParamExtractor {
    using Status = ann_helper::StringParamStatus;
public:
    constexpr static char default_delimiter = ':';

    explicit StringParamExtractor(const char *param_str, const char* delimiter = &default_delimiter)
        : _param_str(pstrdup(param_str)), _delimiter(delimiter) {
    }

    static void validate(const char *param_str, const size_t max_size = UINT32_MAX,
                         const char* delimiter = &default_delimiter) {
        if (!param_str) {
            ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                            errmsg("please provide a valid parameter string for validation")));
        }
        if (!delimiter) {
            ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                            errmsg("please provide a valid pointer to delimiter")));
        }
        char *token = NULL;
        char *context = NULL;
        size_t count = 0;
        char err_msg[128] = {'\0'};
        Status status = Status::SUCCESS;
        char *tmp_param_str = pstrdup(param_str);
        token = strtok_s(tmp_param_str, delimiter, &context);
        while (token != NULL) {
            int64 val = strtol(token, NULL, 10);
            if (val <= 0) {
                sprintf(err_msg, "invalid value '%s' that cannot be converted to a positive number", token);
                status = Status::INVALID_PARAM;
                break;
            }
            if (errno == ERANGE) {
                sprintf(err_msg, "invalid value '%s' that is out of range", token);
                status = Status::OUT_OF_RANGE;
                break;
            }
            ++count;
            token = strtok_s(NULL, delimiter, &context);
        }
        if (count > max_size) {
            sprintf(err_msg, "the number of index magnitudes %lu exceeds the upper limit of %lu",
                             count, max_size);
            status = Status::OUT_OF_MAX_SIZE;
        }
        pfree_ext(tmp_param_str);
        switch (status) {
            case Status::SUCCESS:
                break;
            case Status::INVALID_PARAM:
            case Status::OUT_OF_RANGE:
            case Status::OUT_OF_MAX_SIZE:
                ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("%s", err_msg)));
                break;
            default:
                ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                                errmsg("invalid string parameter status %d", status)));
                break;
        }
    }

    void extract(Vector<size_t> &values) {
        /* aleady validated via static validate() */
        char *token = NULL;
        char *context = NULL;
        token = strtok_s(_param_str, _delimiter, &context);
        while (token != NULL) {
            values.push_back(strtol(token, NULL, 10));
            token = strtok_s(NULL, _delimiter, &context);
        }
    }

    void destroy() {
        if (_param_str) {
            pfree_ext(_param_str);
        }
    }
private:
    char *_param_str{nullptr};
    const char *_delimiter{nullptr};
};
} /* namespace ann_helper */

inline void validate_magnitudes(const char *value) {
    ann_helper::StringParamExtractor::validate(value);
}

#endif /* DISKANN_UTILS_PARAMETERS_H */
