// RUN: %clang-refold-tester-with-lines include_owned_define_two_survivors_after_observed_replacement
int M(void);
int before = M();
#include "define_m_fn.h"
#include "use_m_fn.h"
int after = M();
