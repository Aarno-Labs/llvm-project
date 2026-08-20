// RUN: %clang-refold-tester-relaxed materialized_header_macro_state_payload_and_preserved_source_share_line
// XFAIL: *
// Refusal shape: a materialized header whose replacement line carries both
// B-derived payload naming a live macro and preserved source that requires that
// macro to expand.
//
// Reaches `reason=MacroStateNotStabilizable`, which nothing else in the suite
// reaches, from the materialized-header macro-state replay in
// `RefoldMacroStateProof`.
//
// This is `materialized_header_macro_state_undefines_and_restores_across_observer`
// with four tokens added: the patched line ends `+ VAL` as well as beginning
// `ID(4242)`.  The undef/restore repair that carries the other test cannot serve
// this line, because the line now needs `VAL` to mean two different things --
// the literal token B asked for inside the invocation, and the value 99 that the
// preserved `+ VAL` requires.  Undefining across the whole line breaks the
// second; not undefining breaks the first.
//
// Moving the second `VAL` off that line to the header suffix folds, which is
// what isolates the failure to the *line*, not to the observation.
//
// TRIAGE: incompleteness, and the one the handoff already names -- the repair is
// sound only over B-derived payload, and nothing currently records which bytes of
// a replacement are B payload and which are preserved source.  Refusing is
// correct until that partition exists; the expected refold below is what the
// partition buys: the undef and its restore bracket only the payload, so the
// preserved `+ VAL` on the same line still sees the definition.
//
// The expected refold spends two physical lines inside the expression to do it.
// That is legal -- a directive may sit between tokens of a declaration -- and it
// is why this needs a partition rather than a wider bracket.
#define VAL 99
int crosser(void) { return VAL; }
#define ID(x) (x)
int patched(void) { return (
#undef VAL
VAL
#define VAL 99
) + VAL; }
int tu(void) { return 0; }
