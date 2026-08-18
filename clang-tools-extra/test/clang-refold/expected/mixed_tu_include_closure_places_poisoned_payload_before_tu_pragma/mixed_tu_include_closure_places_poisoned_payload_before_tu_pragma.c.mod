// Companion to the payload-insensitivity theorem: the same straddling shape,
// but the edited payload names the poisoned identifier.
//
// Observing a preserved directive's state is not the same as having no place to
// go. A zone is placeable whenever the sides that are both legal and reproduce B
// number exactly one, and observation is one of the ways that set shrinks to
// one rather than a reason to give up.
//
// `GCC poison` is consumed by the preprocessor and emits no token, so both
// placements produce the same token stream and the difference between them is
// purely legality: naming a poisoned identifier after the directive is an error,
// naming it before is not. The payload is therefore realizable on exactly one
// side, and it is the side a determined zone already takes -- so the partition
// commits to nothing it was not already committing to, and does so by proof.
//
// This previously refused and surrendered the whole translation unit, on the
// reasoning that the closure "must refuse rather than commit the placement that
// happens to compile". Under the rule that B is the specification (CLAUDE.md
// §9), the placement that reproduces B is not one that happens to compile: it
// is the only one that satisfies the contract.
//
// The restriction to `poison` is the point. A `MacroStateStack` zone is legal on
// both sides and merely expands differently, so its side is decided by which one
// reproduces B, which needs macro-liveness facts the classification does not
// carry; it still refuses. So does an unclassified directive.
//
// RUN: rm -rf %t.dir && mkdir -p %t.dir/headers
// RUN: cp %s %t.dir/mixed_tu_include_closure_places_poisoned_payload_before_tu_pragma.c
// RUN: cp %S/headers/two.inc %S/headers/two.inc %t.dir/headers/
//
// RUN: cd %t.dir && clang -E -P -I headers --refold-map=%t.dir/map.json mixed_tu_include_closure_places_poisoned_payload_before_tu_pragma.c -o %t.dir/a.i
// RUN: diff -u %S/expected/mixed_tu_include_closure_places_poisoned_payload_before_tu_pragma/mixed_tu_include_closure_places_poisoned_payload_before_tu_pragma.c.i %t.dir/a.i
//
// RUN: cd %t.dir && clang-refold --no-lines --strict --verify-output=fatal --log-level=trace --pp %t.dir/a.i --pp-mod %S/expected/mixed_tu_include_closure_places_poisoned_payload_before_tu_pragma/mixed_tu_include_closure_places_poisoned_payload_before_tu_pragma.c.i.mod --refold-map %t.dir/map.json --out %t.dir/a.c.mod 2>&1 | FileCheck %s
//
// The payload is placed by proof, and the pragma survives where it was written.
// CHECK: observes the preserved directive's PoisonIdentifiers state
// CHECK-SAME: admits only the side before it
// CHECK-SAME: by proof rather than by preference
//
// This is a preservation case, so it pins its refolded source rather than only
// asserting a diagnostic. The expectation is the whole emitted translation
// unit, which is what makes a later change to placement, to the preserved
// pragma's position, or to any surviving comment a reviewable diff.
// RUN: diff -u %S/expected/mixed_tu_include_closure_places_poisoned_payload_before_tu_pragma/mixed_tu_include_closure_places_poisoned_payload_before_tu_pragma.c.mod %t.dir/a.c.mod
#define KEEP(x) ((x) + 1)

int untouched = KEEP(5);

int arr[] = { FOO
#pragma GCC poison FOO
};
