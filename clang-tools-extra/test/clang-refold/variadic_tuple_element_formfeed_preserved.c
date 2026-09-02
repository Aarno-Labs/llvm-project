// RUN: %clang-refold-tester variadic_tuple_element_formfeed_preserved
// Regression: a variadic tuple element whose leading whitespace includes a form
// feed must keep that form feed in the refolded source.  The element rewrite
// replaces the element's trimmed byte range, so the trim must recognize the
// full C preprocessing whitespace set -- space, HT, LF, VT, FF, CR -- and not
// just the four characters a hand-rolled trim happened to list.  Trimming only
// four silently swallowed the form feed into the replaced range.
#define SEMI_LIST_1(a)          a
#define SEMI_LIST_2(a, b)       a; b
#define SEMI_LIST_3(a, b, c)    a; b; c
#define SEMI_LIST_4(a, b, c, d) a; b; c; d

#define SEMI_LIST_GET_MACRO(_1, _2, _3, _4, NAME, ...) NAME

#define SEMI_LIST(...) __VA_OPT__(     SEMI_LIST_GET_MACRO(__VA_ARGS__, SEMI_LIST_4, SEMI_LIST_3, SEMI_LIST_2, SEMI_LIST_1)(__VA_ARGS__) )

SEMI_LIST(int x = 1, int y = 2, int z = 3);
