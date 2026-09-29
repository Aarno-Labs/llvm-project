// RUN: %clang-refold-tester-clang-flags forced_include_edited_stream_replay_omits_prefix -- -include %S/headers/forced_include_shadowing_prelude.h
//
// Regression: the closing check and `--check` replay the edited stream without
// the producer's `-include` files.
//
// A forced include is entered ahead of the first byte of this file, so the
// preprocessed stream -- and the edited stream derived from it -- already
// carries the prelude's declarations.  Replaying `-include` over the edited
// stream re-read `extern int prelude_reset(void);` under the prelude's own
// `prelude_reset()` macro, a malformed invocation, so the edited stream could
// not be preprocessed: `--verify-output=fatal` shipped the refold unverified
// and `--check` failed.
int counter = 2;
int main(void) { return prelude_reset() + counter; }
