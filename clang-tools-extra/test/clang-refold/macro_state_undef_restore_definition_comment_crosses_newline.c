// RUN: %clang-refold-tester-verify-off macro_state_undef_restore_definition_comment_crosses_newline
//
// Regression: the restore after a synthetic `#undef` re-emits the whole
// definition when its trailing block comment crosses a newline, a shape common
// in system headers.  The restore copies the definition's source bytes by the
// producer's directive extent, and the producer used to end that extent at the
// first physical newline, inside the comment.  The output then restored
// `#define ZZ VALUE /* the value, whose note` with the comment unterminated,
// and it swallowed `int later = ZZ;`.  The extent is now where Clang's lexer
// stands after the end-of-directive token.
//
// As in macro_state_payload_names_live_macro_undef_restore, B's `ZZ` is an
// identifier that must survive, `int before` blocks carrying the definition
// past the replacement, and `int later` blocks a bare `#undef`.
#define VALUE 3
#define ZZ VALUE /* the value, whose note
   spans two lines */
int before = ZZ;
int arr[] = { 1, 2 };
int later = ZZ;
