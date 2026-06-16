// RUN: %clang-refold-tester-clang-flags tu_base_file_observer_output_path_sensitive -- -I %S
int p = 3;
const char *v = "tu_base_file_observer_output_path_sensitive.c";
int main(void) { return p; }
