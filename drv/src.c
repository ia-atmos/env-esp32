#include <stddef.h>

#include "drv.h"

static drv_peek_t PEEK[DRV_SRC_MAX];

void drv_source_add (drv_src_t src, drv_peek_t peek) {
    PEEK[src] = peek;
}

drv_src_t drv_next_source (void) {
    drv_src_t best = DRV_SRC_NONE;
    int64_t t0 = 0;
    for (int i=1; i<DRV_SRC_MAX; i++) {
        int64_t t;
        if (PEEK[i]!=NULL && PEEK[i](&t) && (best==DRV_SRC_NONE || t<t0)) {
            best = (drv_src_t) i;
            t0 = t;
        }
    }
    return best;
}
