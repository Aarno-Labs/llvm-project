// RUN: %clang-refold-tester-with-lines include_owned_define_two_survivors_after_observed_replacement
int M(void);
#include "define_m_fn.h"
int before = M();
#include "use_m_fn.h"
int after = M();
