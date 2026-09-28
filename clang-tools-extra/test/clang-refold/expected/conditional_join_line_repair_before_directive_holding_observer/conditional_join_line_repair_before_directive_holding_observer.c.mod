// RUN: %clang-refold-tester-with-lines-verify-off conditional_join_line_repair_before_directive_holding_observer
//
// The edit inside the `#if` arm adds a line, so the first line-state observer
// after the join needs a synthetic `#line`.  That observer is the `__LINE__`
// operand of the `#line` below, on the directive's continuation line.
//
// It is expanded before its directive takes effect, so it sees the state at
// the directive's own line start, and a repair can only go before the whole
// directive.  The repair used to be placed at the observer's physical line,
// splicing `#line \` onto `#line 7 "..."`, which does not preprocess.
#define ON 1
#if ON
int a = 2;
#endif
int c = 3;
#line 15 "conditional_join_line_repair_before_directive_holding_observer.c"
#line \
  __LINE__
int y = __LINE__;
