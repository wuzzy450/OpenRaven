// Original compatibility code; contains no game shader source.
#ifndef RAVEN_GLSL_COMPAT_H
#define RAVEN_GLSL_COMPAT_H
#include <ctype.h>
#include <stdio.h>
#include <string.h>

// The arcade loader requests a core profile but some effects use legacy sampler
// function names. Preserve source length/line numbers and leave compatibility
// shaders, comments and longer identifiers untouched.
static int raven_glsl_compat(char *src)
{
    int version = 0;
    char profile[32] = {0};
    char *p = src;
    while (isspace((unsigned char)*p)) ++p;
    if (sscanf(p, "#version %d %31s", &version, profile) != 2 ||
        version < 150 || strcmp(profile, "core")) return 0;
    int changed = 0;
    while (*p) {
        if (p[0] == '/' && p[1] == '/') {
            while (*p && *p != '\n') ++p;
        } else if (p[0] == '/' && p[1] == '*') {
            p += 2;
            while (*p && !(p[0] == '*' && p[1] == '/')) ++p;
            if (*p) p += 2;
        } else if (isalpha((unsigned char)*p) || *p == '_') {
            char *start = p;
            while (isalnum((unsigned char)*p) || *p == '_') ++p;
            size_t n = (size_t)(p - start);
            char *next = p;
            while (isspace((unsigned char)*next)) ++next;
            if (*next == '(' && ((n == 9 && !memcmp(start, "texture2D", 9)) ||
                               (n == 11 && !memcmp(start, "textureCube", 11)))) {
                memcpy(start, "texture", 7);
                memset(start + 7, ' ', n - 7);
                ++changed;
            }
        } else ++p;
    }
    return changed;
}
#endif
