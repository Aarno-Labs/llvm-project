// RUN: %clang-refold-tester alignment_window_structure_repair_keeps_directive_inside_deleted_initializer
// RUN: FileCheck --input-file=%t/outputs/alignment_window_structure_repair_keeps_directive_inside_deleted_initializer.out %s
//
// Regression: when the structure-respecting repair commits a deletion that
// must straddle a `#define`, the deletion is realized around the directive,
// which stays in place.
//
// The edit deletes both tables.  B keeps only `; } ;` of `struct s` and the
// `static` of `f`, so the deletion has 24 optimal alignments.  Every one of
// them straddles the `#define` inside `t1`'s initializer and seven also
// straddle the `#undef`; of the rest, only the map deleting from `t1`'s
// `static` through `t2`'s `;` removes whole source lines.
//
// The structural tiler then sees two ways to delete `t1`: delete around the
// `#define`, keeping it, or cut through the `NUL` invocation and consume the
// directive with the tokens around it.  That tie used to be declined, and the
// hunk fell back to one span that dropped `#define NUL`; consuming it also
// left the realization's suffix state unknown, so the repair could not even
// accept the map.  Inside the window the repair committed, the partition
// keeping the directive is selected, and the map is accepted.
//
// CHECK: window 0: sub-rectangle A=[10,48) B=[10,14) keeps 1 of its 24 optimal map(s) under the structure-respecting keys
// CHECK: lies in a window the structure-respecting repair committed; selecting the partition that keeps the most protected structure in place
// CHECK: window 0: candidate map 1 of 1 realized: accepted
// CHECK: window 0 committing structure-respecting repair: declining requests terminal fallback
// CHECK: lies in a window the structure-respecting repair committed; selecting the partition that keeps the most protected structure in place
struct s { const char *n; int v; };
static const struct s t1[] = {
#define NUL "", 0
	{ NUL },
};
/* c */
static const struct s t2[] = {
	{ NUL },
};
#undef NUL
static int f(void) { return 0; }
