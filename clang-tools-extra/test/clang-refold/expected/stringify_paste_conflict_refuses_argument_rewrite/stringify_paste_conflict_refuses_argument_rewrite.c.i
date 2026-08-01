struct fld { const char *name; unsigned long len; int (*fn)(void); };
int parse_mime(void);
int parse_apple(void);
struct fld bang[] = {
 { "mime", sizeof("mime") - 1, parse_mime },
 { "apple", sizeof("apple") - 1, parse_apple },
 { 0, 0, 0 }
};
