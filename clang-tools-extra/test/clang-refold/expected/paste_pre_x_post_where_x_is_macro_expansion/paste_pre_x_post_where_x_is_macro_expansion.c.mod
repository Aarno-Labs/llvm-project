// RUN: %clang-refold-tester-with-lines paste_pre_x_post_where_x_is_macro_expansion

// test101.c: pre_##X##_post where X is itself a macro expansion.
#define CAT(a,b) a##b
#define XCAT(a,b) CAT(a,b)

#define WRAP(x) x
#define SEG XCAT(in,ner)

#define PASTE3_I(pre,x,post) pre##x##post
#define PASTE3(pre,x,post)   PASTE3_I(pre, x, post)
int pre_outer_post (void) { return 5; }

int main(void) {
  return pre_outer_post() == 5 ? 0 : 1;
}
