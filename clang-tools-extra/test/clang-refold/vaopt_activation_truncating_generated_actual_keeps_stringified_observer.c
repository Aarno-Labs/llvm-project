// RUN: %clang-refold-tester-verify-off vaopt_activation_truncating_generated_actual_keeps_stringified_observer
// Supplying an omitted variadic actual fails closed when it would end a
// generated call's actual early and an unchanged token observes that actual.
//
// `FWD` spells `G(X __VA_OPT__(, __VA_ARGS__) + 1)`.  With no variadic actual
// the group outputs nothing and `FIELD_OPT_STR`'s `name` is `alpha + 1`;
// with one, the group's `,` ends `name` at `alpha` and `+ 1` moves into the
// variadic actual.  The producer's `arg_token_deps` therefore make `name`
// depend on `FWD`'s variadic formal although the group outputs nothing
// before its `,`.  B appends through the `__VA_OPT__` tail of the generated
// callee but keeps `"alpha + 1"`, which the rewrite would re-spell as
// `"alpha"`, so no variadic actual is supplied and B's appended tokens are
// inserted in source after the invocation.
#define APPLY(F, G, X, ...) F(G, X, __VA_ARGS__)
#define FWD(G, X, ...) G(X __VA_OPT__(, __VA_ARGS__) + 1)
#define FIELD_OPT_STR(name, ...) #name __VA_OPT__(":" #__VA_ARGS__)
const char *s = APPLY(FWD, FIELD_OPT_STR, alpha);
