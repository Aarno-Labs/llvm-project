// RUN: %clang-refold-tester xmacro1

// 1. Define the list once using a placeholder macro 'X'
#define ERROR_LIST \
  X(ERR_NONE,    "No error") \
  X(ERR_TIMEOUT, "Connection timed out") \
  X(ERR_OOM,     "Out of memory")

// 2. Generate the ENUM
#define X(id, label) id,  // Redefine X to just grab the ID
enum ErrorCode {
  ERROR_LIST
};
#undef X

// 3. Generate the String Array
#define X(id, label) label, // Redefine X to just grab the label
const char* error_messages[] = {
  ERROR_LIST
};
#undef X
