#include <stdarg.h>
#include <stdio.h>

#include "drv.h"

static char ERR[256];

const char* drv_errf (const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(ERR, sizeof(ERR), fmt, ap);
    va_end(ap);
    return ERR;
}
