// RUN: %clang-refold-tester-relaxed-verify-off relaxed_stringify_operand_outside_stringize_image_expands
// Soundness guard: a stringified operand B spells with leading or trailing
// white space is outside the image of `#`, so no argument reproduces it.
//
// `#` deletes white space before the argument's first preprocessing token and
// after its last (C 6.10.3.2), so `"q "` is a literal stringification can never
// produce.  Folding the callsite as `S(<anything>)` therefore cannot replay B:
// whatever argument is chosen, re-expansion regenerates a literal with no
// trailing space.  The fold must be refused and the callsite realized by whole
// cover, which reproduces B exactly.
//
// The relaxed pipeline used to admit it.  Its standard args-only path never
// compared against Clang's own stringization at all -- the authoritative
// round-trip was strict-only, and the paste path's operand check is reached
// only when a paste derived the argument.  Every relaxed check that did run
// compares canonicalized inverse payloads *trimmed*, which makes leading and
// trailing white space invisible to all of them.  So this emitted `S(q)`,
// regenerating `"q"`, and exited 0 at the tool's default verify level.
//
// The relaxation itself is unchanged: a stringified operand may still disagree
// when B left it exactly as A spelled it, because that position records no edit
// and re-expansion regenerates it.  This literal is not that -- B edited it to
// something unreachable -- and the two cases are pinned by
// assert_stale_stringified_arg_relaxed.c and
// non_strict_stringify_macro_arg_edit.c respectively.
//
// Verify-off on purpose: under `fatal` the closing check re-preprocesses the
// output and refuses the bad fold itself, so the test would pass whether or not
// the admission rule holds.  The pinned `.c.mod` is a statement about the
// planner.
#define S(x) #x
const char *v = S(q);
