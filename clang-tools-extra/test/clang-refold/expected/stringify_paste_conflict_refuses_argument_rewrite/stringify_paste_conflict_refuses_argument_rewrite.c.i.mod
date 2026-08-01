struct fld { const char *name; unsigned long len; int (*fn)(void); };
int parse_mime_xjtr_0(void);
int parse_apple_xjtr_0(void);
struct fld bang_xjtr_0[] = {
 { "mime", sizeof("mime") - 1, parse_mime_xjtr_0 },
 { "apple", sizeof("apple") - 1, parse_apple_xjtr_0 },
 { 0, 0, 0 }
};
