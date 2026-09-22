// RUN: %clang-refold-tester deleted_declarations_keep_comment_between_them
//
// B deletes every token, so the whole file is one deletion hunk.  The comment
// between the two declarations has no tokens and B, being preprocessed, cannot
// say whether it should go too.  Which declaration it documents is unknown, so
// the refold keeps it rather than guess: the hunk is split at the comment's
// gap and each declaration is deleted on its own.
//
// Before this was fixed, the deletion replaced every byte from `int` to the
// final `;`, so the comment was dropped.  Deleting only `foo` kept it, because
// that edit ended before the comment -- the outcome depended on the edit's
// extent, not on any rule.
int foo();
// SOME COMMENT IN BETWEEN
char bar();
