// RUN: %clang-refold-tester-clang-flags materialized_header_base_file_observer -- -I %S
#include "headers/parent5.h"
int main(void) { return p + q; }
