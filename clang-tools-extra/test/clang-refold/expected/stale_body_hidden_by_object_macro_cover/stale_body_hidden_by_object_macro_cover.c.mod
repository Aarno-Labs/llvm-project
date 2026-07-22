// RUN: %clang-refold-tester stale_body_hidden_by_object_macro_cover
#define T 1

static int text_chars_xjtr_0[4] = {0, 1, 1, 1};

#define LOOKS(NAME, COND) \
  static int looks_ ## NAME(const unsigned char *buf) { \
    int t = text_chars[buf[0]]; \
    return !(COND); \
  }

static int looks_ascii_xjtr_0(const unsigned char *buf) { int t = text_chars_xjtr_0[buf[0]]; return !(t != 1); }
