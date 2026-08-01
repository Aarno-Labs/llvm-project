// RUN: %clang-refold-tester-with-lines line_resync_precedes_header_macro_line_observer
// An edit that inserts a physical line must discharge its `#line` resync
// before the first preserved line observer it displaces, not merely before
// the untouched suffix that follows the replacement.
//
// `RF_MARK` is defined in common.h and consumes `__LINE__`, so the observer is
// reached only through the macro expansion; its value is fixed by the callsite
// in this TU.  The inserted declaration lands mid-line, after the callsite
// line's leading indentation, so the replacement ends in a newline plus an
// indentation-only run.  Refusing the local injection there leaves the resync
// to be flushed at the next safe BOL in the emitted output, which is already
// past `RF_MARK(alpha)`, and the observer expands one line too high.
//
// Regression for the dbcc `2json.c` / `2xml.c` `__LINE__` off-by-one.
#include "common.h"

static void probe(void)
{
	int before = 1;
	RF_MARK(alpha)
	int after = 2;
}
