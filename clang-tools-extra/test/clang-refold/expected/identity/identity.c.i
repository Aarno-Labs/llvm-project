typedef enum {
  ERR_OK = (0), ERR_NOT_FOUND = (404), ERR_BAD = (500),
} ErrorCode;
static const char* error_name(ErrorCode e) {
  switch (e) {
    case ERR_OK: return "OK"; case ERR_NOT_FOUND: return "NOT_FOUND"; case ERR_BAD: return "BAD";
    default: return "UNKNOWN";
  }
}
int main (void) { fprintf(stderr, "[%s:%d] " "hello %s\n" , "world", "identity.c", 68); do { if (!((( (3) < (7) ) ? (3) : (7)) == 3)) { fprintf(stderr, "[%s:%d] " "ASSERT failed: %s\n" , "MIN(3, 7) == 3", "identity.c", 70); } } while (0); do { int x = (42); { fprintf(stderr, "[%s:%d] " "x=%d str(x)=%s\n" , x, "x", "identity.c", 73); }; } while (0); printf("%s @ %s:%d\n", ("here"), "identity.c", 76); printf(("file=%s line=%d extra=%d\n"), "identity.c", 79 , 123); int tmp_0 = (10); int tmp_1 = (20); ErrorCode e = ERR_NOT_FOUND; fprintf(stderr, "[%s:%d] " "error=%s (%d)\n" , error_name(e), (int)e, "identity.c", 85); return 0;}
