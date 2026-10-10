// ERROR: no member
struct S { int x; };
int main() { struct S s; s.y = 1; return 0; }
