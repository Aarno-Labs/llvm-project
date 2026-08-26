// RUN: %clang-refold-tester-with-lines source_line_directive_after_apostrophe_in_line_comment
//
// Regression: recovering source line-control state must scan past a line
// comment that contains a quote character.
//
// THE TRIGGER IS THE APOSTROPHE ON THE NEXT LINE.  Keep exactly one quote
// character in this comment block.  Removing it, or adding a second one,
// disarms this test: the scan then succeeds for the ordinary reason and the
// test passes without exercising anything.
//
// don't
//
// LogicalLocationAtOffset answers a preprocessor question -- after executing
// every source directive before this byte, what logical file and line are
// active? -- by scanning the whole source prefix in order.  Its per-line
// collector recognized string literals, character literals, and block
// comments, but had no line-comment arm.  So the apostrophe above opened a
// character literal that no closing quote ended, the collector reached the
// newline and failed the line, and its caller abandoned the entire prefix
// scan.
//
// Everything after that comment became invisible, including the #line
// directive below it.  The recovered location fell back to the physical file,
// the repair emitted a directive naming this file instead of the virtual one,
// __LINE__ re-expanded to its physical value, and assembly verification
// refused the translation unit and emitted raw B.
//
// Comments are not recognized inside literals, and the converse holds too: a
// quote inside comment prose is comment text.  Apostrophes in English comments
// are ordinary in real C, so this cost a preserved refold for a reason that
// had nothing to do with the edit.
//
// The character literal below is real and must still be read as one, and the
// #line state above it must survive the comment: the insertion moves the
// observer, so the repair has to resync it to logical line 81 of the virtual
// file.  A regression re-emits a directive naming this file instead.
#line 80 "virtual_unit.c"
char c = 'a';
int added = 0;
#line 81 "virtual_unit.c"
int observed = __LINE__;
const char *unit = __FILE__;
