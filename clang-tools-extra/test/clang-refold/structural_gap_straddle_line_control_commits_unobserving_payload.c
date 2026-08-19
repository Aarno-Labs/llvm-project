// RUN: %clang-refold-tester structural_gap_straddle_line_control_commits_unobserving_payload
// Regression: one B token replacing material on both sides of a `#line` is
// committed after the directive when it observes no logical position.
//
// A `#line` shifts what `__LINE__`, `__FILE__`, `__FILE_NAME__` and
// `__BASE_FILE__` report for everything after it, so a payload's side of one is
// fixed as soon as the payload observes any of them.  That makes the placement
// undecidable in general and admissible in exactly the narrow case this pins:
// text observing none of the four.
//
// The question is not answered by spelling alone.  An identifier in the payload
// that is itself a live macro can reach an observer its replacement list names
// and the payload never spells, so the macro-state proof closes over recorded
// replacement lists to decide it.  Here the payload is a numeric constant, the
// gap-crossing proof reports `proof=LineControlUnobserved`, and the directive
// keeps its own line.
int arr[] = { 1,
#line 100
2 };
