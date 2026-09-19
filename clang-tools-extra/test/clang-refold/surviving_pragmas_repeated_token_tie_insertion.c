// RUN: %clang-refold-tester-verify-off surviving_pragmas_repeated_token_tie_insertion
// An insertion between two surviving pragmas next to a repeated token.
//
// `,` occurs on both sides of the inserted `9 ,`, so which `,` is original is
// a tie.  The certified-window pairing keyed `pack(1)` off one arbitrary
// alignment and saw it move, turning it into a delete-and-insert that printed
// the directive twice.  Each pragma text is unique among the unpaired lines,
// so the second-chance pairing keeps both, and the placement check puts the
// payload between them.
int arr[] = { 1,
#pragma pack(1)
#pragma pack(2)
2 };
