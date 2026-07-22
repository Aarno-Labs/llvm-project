static int text_chars[4] = {0, 1, 1, 1};
static int looks_ascii(const unsigned char *buf) { int t = text_chars[buf[0]]; return !(t != 1); }
