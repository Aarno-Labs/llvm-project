// RUN: %clang-refold-tester-with-lines token_paste_ambiguous_adjacent_duplicates_no_guess

// test73: Designed ambiguity / no-guess scenario: duplicated pasted tokens adjacent.
#define DUP(T) int tok_##T tok_##T;
int tok_int tok_int2;
int main(){ return 0; }
