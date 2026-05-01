/**
 * Copyright ...
 */

#ifndef INDEX_INSPECT_H
#define INDEX_INSPECT_H

#include <utility>

#include "postgres.h"
#include "fmgr/fmgr_comp.h"
#include "utils/builtins.h"
#include "access/annvector/module/size_format.h"

struct IndexInspectResult : public BaseObject {
    size_t nattr{0};
    size_t capacity{0};
    Datum *attributes{NULL};
    Datum *contents{NULL};
    void destroy();
    void append(IndexInspectResult &&other);

    void append_attr(const char *msg)
    {
        expand();
        attributes[nattr] = PointerGetDatum(cstring_to_text(msg));
    }
    template <typename... Args>
    void append_attr(const char *msg, Args &&...args)
    {
        expand();
        attributes[nattr] = get_data(msg, std::forward<Args>(args)...);
    }

    void fill_content(const char *msg)
    {
        contents[nattr] = PointerGetDatum(cstring_to_text(msg));
        ++nattr;
    }
    template <typename... Args>
    void fill_content(const char *msg, Args &&...args)
    {
        contents[nattr] = get_data(msg, std::forward<Args>(args)...);
        ++nattr;
    }
    void fill_content(size_t bytes)
    {
        auto sf = ann_helper::format_size(bytes);
        if (trunc(sf.n) == sf.n) {
            contents[nattr] = get_data("%lu %s", size_t(sf.n), sf.unit_str());
        } else {
            contents[nattr] = get_data("%.3f %s", sf.n, sf.unit_str());
        }
        ++nattr;
    }
private:
    void expand()
    {
        if (capacity <= nattr) {
            const size_t new_capacity = Max(8ul, nattr * 2ul);
            if (capacity == 0) {
                attributes = (Datum *)palloc(sizeof(Datum) * new_capacity);
                contents = (Datum *)palloc(sizeof(Datum) * new_capacity);
            } else {
                attributes = (Datum *)repalloc(attributes, sizeof(Datum) * new_capacity);
                contents = (Datum *)repalloc(contents, sizeof(Datum) * new_capacity);
            }
            capacity = new_capacity;
        }
    }

    template <typename... Args>
    Datum get_data(const char *msg, Args &&...args)
    {
        constexpr int init_size = 128;
        int size = init_size;
        char *msg_buf = (char *)palloc(size);
        int n;
        do {
            n = snprintf(msg_buf, size, msg, std::forward<Args>(args)...);
            if (n < 0) {
                pfree(msg_buf);
                ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
                                errmsg("Failed to parse format text")));
            }
            if (n >= size) {
                size *= 2;
                msg_buf = (char *)repalloc(msg_buf, size);
            }
        } while (n >= size);
        text *t = cstring_to_text_with_len(msg_buf, n);
        pfree_ext(msg_buf);
        return PointerGetDatum(t);
    }
};
extern Datum index_inspect_oid(PG_FUNCTION_ARGS);
extern Datum index_inspect_name(PG_FUNCTION_ARGS);

#endif /* INDEX_INSPECT_H */
