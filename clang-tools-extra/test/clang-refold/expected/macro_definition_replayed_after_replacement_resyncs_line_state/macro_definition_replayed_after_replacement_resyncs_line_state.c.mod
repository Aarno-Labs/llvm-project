// RUN: %clang-refold-tester-with-lines macro_definition_replayed_after_replacement_resyncs_line_state
// A consumed definition replayed after a replacement must not shift the line
// state of the untouched source after it.
//
// The edit deletes `grammar` and rewrites both prototypes.  Its `;` and
// `const char *` tokens are ambiguous, so the replacement starts after
// `lead`'s parameter list and ends in the middle of the `get_grammar` line.
// `FIELD_LIST` lies inside the replaced bytes and is still used by `fields`,
// so its definition is replayed after the replacement.  The replacement's own
// line resync was decided before that replay, so the definition's lines
// followed it uncorrected and shifted the line `__LINE__` reads below.
//
// The replayed block is resynced as an insertion at the edit's end, which
// restores the location the untouched suffix already had.  Regression for
// tenjin's howerj/dbcc `parse.c` (`X_MACRO_PARSE_VARS` before an `assert`).
static int lead(int *ctx, int x);
const char *
#define FIELD_LIST\
	X(alpha)\
	X(beta)\
	X(gamma)
get_grammar(int *ctx)
#line 26 "macro_definition_replayed_after_replacement_resyncs_line_state.c"
{
	return "g";
}

#define X(n) int n;
struct fields { FIELD_LIST };
#undef X

int line_after(void) { return __LINE__; }
static int lead(int x) { return x; }
