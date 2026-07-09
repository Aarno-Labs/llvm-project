// RUN: %clang-refold-tester pragma_directive_pack_edit_after
#pragma pack(1)
int a = 1;
#pragma pack()
int b = 2;
