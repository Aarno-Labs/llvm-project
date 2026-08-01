// RUN: %clang-refold-tester stringize_escaped_char_arg_strict_folds
//
// Strict-mode refold of a stringify-only macro argument whose edited value
// contains an escape sequence.
//
// The argument of STR is used only stringified (`#x`).  The modified stream
// edits the stringified value from "ab" to "a\tb" (adding a `\t` escape).  The
// fold STR(a\tb) re-preprocesses back to "a\tb" exactly, so it is B-faithful and
// strict mode must produce it.
//
// Regression: strict verified the fold by re-deriving `#arg` with a blanket
// C-string escape (quoteCString), which doubled the stray backslash
// (`"a\\tb"`) and so did not match Clang's actual `#(a\tb)` == `"a\tb"`.  Strict
// therefore wrongly rejected the fold and expanded.  The verifier now models
// Clang's `#` operator exactly (escaping `\`/`"` only inside string/char
// literals, stray backslashes verbatim), so strict folds this correctly and
// identically to relaxed mode.
#define STR(x) #x
const char *s = STR(ab);
