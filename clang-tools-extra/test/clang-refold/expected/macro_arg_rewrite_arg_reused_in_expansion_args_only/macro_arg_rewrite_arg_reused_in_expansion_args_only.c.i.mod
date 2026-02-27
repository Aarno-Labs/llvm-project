struct S64 { float field_a; float field_b; };
int main() { struct S64 s; s.field_a = 1; s.field_b = 2; return s.field_a + s.field_b; }
