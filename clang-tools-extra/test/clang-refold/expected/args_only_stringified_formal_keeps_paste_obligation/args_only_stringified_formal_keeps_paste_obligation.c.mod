// RUN: %clang-refold-tester-verify-off args_only_stringified_formal_keeps_paste_obligation
// A stringified formal still owes its pasted occurrences to the args-only proof.
//
// B edits the standard and stringified uses of `name` but leaves the pasted
// `f_before` and `v_before` alone.  Rewriting the actual to `after` would
// re-expand both pastes, so no args-only fold realizes B and the invocation is
// expanded.  Strict occurrence replay used to skip every paste span of a
// stringified formal, and emitted `ENTRY(after)` at verify-off.
static int before = 1;
static int after = 2;
#define ENTRY(name) static const char *f_##name = #name; static int v_##name = name;
static const char *f_before = "after"; static int v_before = after;
int main(void) { return v_before != 2; }
