// RUN: %clang-refold-tester stale_body_hidden_by_object_macro_cover
#define T 1

static int text_chars[4] = {0, 1, 1, 1};

#define LOOKS(NAME, COND) \
  static int looks_ ## NAME(const unsigned char *buf) { \
    int t = text_chars[buf[0]]; \
    return !(COND); \
  }

LOOKS(ascii, t != T)
