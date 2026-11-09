// CHECK for the installer's host tests in C.
#pragma once

#include <stdio.h>

static int checks, failures;

#define CHECK(cond)                                                                    \
    do {                                                                               \
        checks++;                                                                      \
        if (!(cond)) {                                                                 \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);   \
            failures++;                                                                \
        }                                                                              \
    } while (0)

static inline int check_report(const char *name)
{
    printf("%s: %d checks, %d failures\n", name, checks, failures);
    return failures ? 1 : 0;
}
