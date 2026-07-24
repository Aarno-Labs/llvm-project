// RUN: %clang-refold-tester semantic_alignment_stale_object_macro_body_after_owner_edit
// Regression: an object-like macro cover must not retain a function-like
// invocation whose body still names a TU object that was renamed by the edit.
int prefix_marker = 17;
#define EXPECTED_FLAG 1
static int character_classes[4] = {0, 1, 1, 1};
#define DEFINE_CLASSIFIER(NAME, CONDITION) \
  static int classify_ ## NAME(const unsigned char *buffer) { \
    int flag = character_classes[buffer[0]]; \
    return !(CONDITION); \
  }
DEFINE_CLASSIFIER(ascii, flag != EXPECTED_FLAG)
int suffix_marker = 23;
