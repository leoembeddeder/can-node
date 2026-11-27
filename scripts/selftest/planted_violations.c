/* SPDX-License-Identifier: MIT */
/*
 * Deliberately wrong code. It is never compiled into anything.
 *
 * scripts/static-analysis.sh sends this file through the same analyser commands as the
 * project's code (same options, same compile flags, same suppression list) and requires each
 * analyser to report the defect planted for it. An analysis that says "no findings" proves
 * nothing unless the same invocation is known to find something; this file is that proof,
 * checked on every run.
 *
 * The file compiles without a warning on purpose: a compiler diagnostic would stop the static
 * analyser before it gets to the planted defect.
 */
#include <stddef.h>

/* cppcheck must report: arrayIndexOutOfBounds (an error) */
static int planted_out_of_bounds(void)
{
    int values[4] = {0, 1, 2, 3};
    int sum = 0;

    for (int i = 0; i <= 4; i++) {
        sum += values[i];
    }

    return sum;
}

/* cppcheck must report: duplicateExpression (a style finding: that class is enabled) */
static int planted_duplicate_expression(int x)
{
    return ((x > 3) && (x > 3)) ? 1 : 0;
}

/* the MISRA addon must report: misra-c2012-15.6 (the body of an if is not a compound statement) */
static int planted_missing_braces(int x)
{
    if (x > 0)
        return 1;

    return 0;
}

/* clang-tidy must report: clang-analyzer-core.NullDereference */
static int planted_null_dereference(void)
{
    const int *pointer = NULL;

    return *pointer;
}

int planted_entry(int x);
int planted_entry(int x)
{
    return planted_out_of_bounds() + planted_duplicate_expression(x) + planted_missing_braces(x) +
           planted_null_dereference();
}
