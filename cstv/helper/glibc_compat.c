/* Built as plain C99 (see the Makefile), so strtol and friends below are the old ones: lets cstvd run on
 * glibc older than 2.38 when built against a newer one. C23 only adds "0b" prefixes; nothing here parses those. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

long __isoc23_strtol(const char *s, char **e, int b) { return strtol(s, e, b); }
unsigned long __isoc23_strtoul(const char *s, char **e, int b) { return strtoul(s, e, b); }
long long __isoc23_strtoll(const char *s, char **e, int b) { return strtoll(s, e, b); }
unsigned long long __isoc23_strtoull(const char *s, char **e, int b) { return strtoull(s, e, b); }
int __isoc23_vsscanf(const char *s, const char *f, va_list ap) { return vsscanf(s, f, ap); }
int __isoc23_sscanf(const char *s, const char *f, ...)
{
    va_list ap;
    va_start(ap, f);
    int n = vsscanf(s, f, ap);
    va_end(ap);
    return n;
}
