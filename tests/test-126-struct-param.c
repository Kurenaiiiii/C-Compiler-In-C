// exit: 42
struct S { int v; };
void set(struct S *s, int v) { s->v = v; }
int main() { struct S s; set(&s, 42); return s.v; }
