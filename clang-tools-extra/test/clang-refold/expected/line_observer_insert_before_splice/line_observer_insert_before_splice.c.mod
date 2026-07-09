// RUN: %clang-refold-tester line_observer_insert_before_splice
// Regression (was a miscompile): inserting a source line before a __LINE__
// observer must not corrupt the splice. The tool previously emitted
// "int a = a = <n>;" (duplicated "a ="); it now re-materializes the shifted
// __LINE__ value cleanly.
int pad = 0;
int a = 7;
