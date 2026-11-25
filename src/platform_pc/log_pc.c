#include "platform_pc.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

static void vlog(FILE *f, const char *tag, const char *fmt, va_list ap) {
    fputs(tag, f);
    vfprintf(f, fmt, ap);
    fputc('\n', f);
    fflush(f);
}

void pc_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vlog(stdout, "[prs] ", fmt, ap);
    va_end(ap);
}

void pc_warn(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vlog(stderr, "[prs] warning: ", fmt, ap);
    va_end(ap);
}

void pc_fatal(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vlog(stderr, "[prs] fatal: ", fmt, ap);
    va_end(ap);
    exit(1);
}
