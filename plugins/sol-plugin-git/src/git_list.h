#ifndef SOL_GIT_LIST_H
#define SOL_GIT_LIST_H

#include <math.h>
#include <stddef.h>

#define GIT_LIST_ROW_HEIGHT 28.0f
#define GIT_LIST_OVERSCAN 8u

typedef struct GitListRange {
    size_t first;
    size_t last;
} GitListRange;

/* Return the half-open visible row range with overscan.
 * count: group row count; top: group start relative to the viewport;
 * viewport_height and row_height: resolved layout pixels.
 * Unmeasured layout emits a bounded initial window. */
static inline GitListRange git_list_visible_range(size_t count, float top,
                                                  float viewport_height,
                                                  float row_height)
{
    GitListRange range = {0u, 0u};
    if (count == 0u) return range;
    if (!isfinite(top) || !isfinite(viewport_height) ||
        !isfinite(row_height) || viewport_height <= 0.0f || row_height <= 0.0f) {
        range.last = count < 96u ? count : 96u;
        return range;
    }
    double begin = -(double)top / row_height;
    double end = ((double)viewport_height - top) / row_height;
    begin -= GIT_LIST_OVERSCAN;
    end += GIT_LIST_OVERSCAN;
    range.first = begin <= 0.0 ? 0u : begin >= (double)count ? count : (size_t)begin;
    range.last = end <= 0.0 ? 0u : end >= (double)count ? count : (size_t)end;
    if (range.last < count && end > (double)range.last) ++range.last;
    return range;
}

#endif
