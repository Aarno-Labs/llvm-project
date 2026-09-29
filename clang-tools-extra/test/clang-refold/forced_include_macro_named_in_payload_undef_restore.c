// RUN: %clang-refold-tester-clang-flags-verify-off forced_include_macro_named_in_payload_undef_restore -- -include %S/headers/forced_include_macro_prelude.h
// RUN: %clang-refold-tester-clang-flags forced_include_macro_named_in_payload_undef_restore -- -include %S/headers/forced_include_macro_prelude.h
//
// Regression: a macro defined by a forced include is live where a B payload
// names it, so the payload must be kept from expanding it.
//
// The edited stream adds a declaration of `prelude_limit`, an identifier, while
// the prelude entered by `-include` defines a macro of that name.  A forced
// include has no site in this file, so the liveness audit found no position
// from which its definitions were bound and never looked at them: the payload
// was emitted as written and expanded to `int 10 = 3;`, while the edited
// stream, replayed without forced includes, keeps the identifier.  A forced
// include's definitions are now bound from this file's first byte, and the
// synthetic partition undefines the name around the payload and restores it
// from the prelude's own directive for the read in `limit`.
int first = 1;
int second = 2;
int limit(void) { return prelude_limit; }
