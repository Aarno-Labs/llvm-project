// RUN: %clang-refold-tester-with-lines macro_define_bol_observed_replacement_include_liveness
enum { use_payload = PAYLOAD };
#define PAYLOAD 100

#include "uses_payload.h"
