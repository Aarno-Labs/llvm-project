// RUN: %clang-refold-tester-verify-off paste_arg_ambiguous_origin_refusal_blocks_token_diff_fallback
// Regression: `foo_bar` -> `fo_o_bar` has two origins -- `M(fo_o, bar)` and
// `M(fo, o_bar)` -- because the inserted `_` gives the pasted token a second
// separator, so either contribution can own the extra text.
//
// The witness-backed derivation proves that and refuses.  Its refusal used to
// come back as an empty result, which is indistinguishable from "this
// derivation does not apply", so the caller fell through to the token-level
// single-segment differ.  That path re-decided the same question from the raw
// spelling, attributed the whole diff region to `a` because it was the only
// span overlapping it, and emitted `M(fo_o, bar)` -- one of two origins, with
// the evidence for choosing it already refuted.
//
// The derivation now reports AmbiguousOrigin distinctly and the caller fails
// closed, so the hunk falls back to the expanded token.
//
// The insertion-point proof in the companion test does not cover this shape:
// the edit region here is bounded by common text on both sides, so it is not
// the free-sliding pure insertion that proof reasons about.
//
// Deliberately --verify-output=off: both origins re-expand to the edited
// stream, so a `fatal` variant accepts either and cannot see the refusal.
#define M(a,b) a ## _ ## b
int fo_o_bar = 0;
