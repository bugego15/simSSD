#include "core/assert.h"

#include <stdio.h>
#include <stdlib.h>

void ssd_panic(const char *file, int line, const char *expr)
{
    fprintf(stderr, "\n[PANIC] %s:%d: %s\n", file, line, expr);
    fflush(stderr);
    abort();
}
