// RUN: %clang-refold-tester-relaxed stringify_paste_conflict_refuses_argument_rewrite
// Regression: `name` is used twice by one macro -- stringized with `#` and
// pasted with `##` -- so the two uses constrain the argument independently.
// Only the pasted product was renamed (`parse_mime` -> `parse_mime_xjtr_0`);
// the literal "mime" is untouched in the edited stream.
//
// No argument text reproduces both operands: any argument satisfying the paste
// necessarily rewrites the string literal too.  Deriving the argument from the
// paste alone emitted DECLARE_FIELD(mime_xjtr_0), silently turning "mime" into
// "mime_xjtr_0" and corrupting a table that is matched with memcmp at run time.
// The callsite is therefore not refoldable and must fall back to the expanded
// text.
//
// Deliberately a --relaxed test.  Under --strict the re-preprocess recheck
// catches the corrupted expansion and falls back anyway, so a strict run passes
// with or without the argument-solver fix and proves nothing.  Tenjin refolds
// without --strict, which is the configuration where this shipped.
struct fld { const char *name; unsigned long len; int (*fn)(void); };

int parse_mime(void);
int parse_apple(void);

struct fld bang[] = {
#define	DECLARE_FIELD(name) { # name, sizeof(# name) - 1, parse_ ## name }
	DECLARE_FIELD(mime),
	DECLARE_FIELD(apple),
#undef	DECLARE_FIELD
	{ 0, 0, 0 }
};
