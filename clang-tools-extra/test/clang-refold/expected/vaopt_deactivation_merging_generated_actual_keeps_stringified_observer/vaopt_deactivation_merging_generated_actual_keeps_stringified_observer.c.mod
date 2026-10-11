// RUN: %clang-refold-tester-verify-off vaopt_deactivation_merging_generated_actual_keeps_stringified_observer
// Removing the variadic actual fails closed when it would merge material into
// a generated call's actual and an unchanged token observes that actual.
//
// `FWD` spells `G(X __VA_OPT__(, __VA_ARGS__) + 1)`.  With the variadic
// actual `10` the group's `,` ends `FIELD_OPT_STR`'s `name` at `alpha`;
// without it the group outputs nothing and `+ 1` joins `name`.  The
// producer's `arg_token_deps` therefore make `name` depend on `FWD`'s
// variadic formal.  B drops the `__VA_OPT__` tail of the generated callee but
// keeps `"alpha"`, which removing the actual would re-spell as `"alpha + 1"`,
// so the invocation is expanded.
#define APPLY(F, G, X, ...) F(G, X, __VA_ARGS__)
#define FWD(G, X, ...) G(X __VA_OPT__(, __VA_ARGS__) + 1)
#define FIELD_OPT_STR(name, ...) #name __VA_OPT__(":" #__VA_ARGS__)
const char *s = "alpha";
