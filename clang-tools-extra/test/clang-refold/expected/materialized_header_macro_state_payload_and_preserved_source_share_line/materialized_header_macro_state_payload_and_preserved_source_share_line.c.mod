// RUN: %clang-refold-tester-relaxed materialized_header_macro_state_payload_and_preserved_source_share_line
// Regression: a materialized header whose replacement line carries both
// B-derived payload naming a live macro and preserved source that requires that
// macro to expand.
//
// This is `materialized_header_macro_state_undefines_and_restores_across_observer`
// with four tokens added: the patched line ends `+ VAL` as well as beginning
// `ID(4242)`.  The undef/restore repair that carries the other test brackets the
// replacement's whole physical line, and that bracket cannot serve this one: the
// line needs `VAL` to mean two different things -- the literal token B asked for
// inside the invocation, and the value 99 the preserved `+ VAL` requires.
// Undefining across the whole line breaks the second; not undefining breaks the
// first.  It used to reach `reason=MacroStateNotStabilizable`, which nothing
// else in the suite reaches.
//
// The repair now falls back to a narrower bracket when the line-wide one is
// refused: the `#undef` goes immediately before the replacement and the restore
// immediately after it, so every byte outside the replacement keeps the macro
// state it originally had and is asked for nothing.  The preserved `+ VAL`
// therefore still sees the definition, and the two readings of the name coexist
// on one line.
//
// What the narrow bracket costs is an obligation on the inside.  The bytes it
// reinterprets must be B-derived payload, because B is already fully expanded
// and a live definition named there is a spurious re-expansion; preserved
// source may instead *require* that expansion.  `MaterializedSurface::
// replacementIsWhollyBPayload` certifies it, and is set only for a whole-cover
// realization, whose replacement is the trimmed material of the cover's own B
// tokens.  It is deliberately not `hasOutputByteRange`: the args-only candidate
// for this very invocation emits `ID(VAL)` and sets that range to the whole
// replacement, while `ID(` and `)` there are preserved callsite spelling.  The
// two questions have opposite answers on that patch.
//
// The expected refold spends two physical lines inside the expression.  That is
// legal -- a directive may sit between tokens of a declaration -- and it is why
// this needs a partition rather than a wider bracket.
// Header for the macro-state repair that has no single answer.
//
// `VAL` is observed by `crosser` before the invocation the edit lands on,
// so the definition cannot be carried past the replacement -- and the
// patched line itself also reads `VAL`, so it cannot be undefined across
// either.
#define VAL 99
int crosser(void) { return VAL; }
#define ID(x) (x)
int patched(void) { return 
#undef VAL
(VAL)
#define VAL 99
 + VAL; }
int tu(void) { return 0; }
