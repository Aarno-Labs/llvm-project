// RUN: %clang-refold-tester-with-lines token_paste_run_name_case_change

// test70: run_##name; edit run_open -> run_OPEN should refold to arg rewrite.
#define RUN(name) int run_##name(void){return 0;}
#define CALL(name) run_##name()
RUN(open)
int main() { return CALL(open); }
