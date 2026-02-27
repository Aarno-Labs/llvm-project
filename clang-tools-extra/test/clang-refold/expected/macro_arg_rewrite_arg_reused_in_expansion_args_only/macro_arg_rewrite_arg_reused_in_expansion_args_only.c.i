struct S64 { int field_a; int field_b; };
int main() { struct S64 s; s.field_a = 1; s.field_b = 2; return s.field_a + s.field_b; }
