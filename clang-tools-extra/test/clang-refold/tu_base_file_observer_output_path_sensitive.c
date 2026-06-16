// RUN: %clang-refold-tester-clang-flags tu_base_file_observer_output_path_sensitive -- -I %S
int p = 1;
const char *v = __BASE_FILE__;
int main(void) { return p; }
