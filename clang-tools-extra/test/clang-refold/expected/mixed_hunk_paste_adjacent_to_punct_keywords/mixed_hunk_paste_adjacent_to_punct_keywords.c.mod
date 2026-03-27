// RUN: %clang-refold-tester-with-lines mixed_hunk_paste_adjacent_to_punct_keywords

// test86: Mixed hunk: pasted token adjacent to punctuation/keywords.
#define P(T) int fn_##T(void){return 0;}
P(float)
int main(){ return fn_float(); }
