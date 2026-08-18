// RUN: %clang-refold-tester mixed_tu_closure_commits_insensitive_payload_straddling_consumed_pragma
// A single B token replacing material on both sides of a consumed pragma must
// still preserve the pragma.
//
// The structural tiler splits a replacement hunk at the preserved directive by
// projecting the A seam onto one exact B boundary.  Here the payload is one
// token and the projection returns a range, because `9` could be placed on
// either side: the partition is not unique and the tiler refuses.  Refusing
// surrenders the whole translation unit, which emits `int arr[] = { 9 };` and
// deletes the pragma -- dropping the very directive the refusal protected.
//
// The two placements are equivalent, and provably so rather than by preference.
// `GCC poison` is consumed, so it contributes no token to either stream and
// moving the payload across it changes no token order; and the payload names no
// poisoned identifier, so it cannot observe the state the pragma changes.  With
// both established the tiler commits a side under that proof.
//
// The proof is per effect kind and defaults to observable, so an unrecognized
// pragma keeps refusing, as does a payload that does name the poisoned
// identifier.  It is also restricted to directives the preprocessor consumes:
// a printed pragma such as `message` is spelled into both streams, so the
// payload's side of it is token order and is fixed by the alignment.
int arr[] = { 
#pragma GCC poison FOO
9 };
