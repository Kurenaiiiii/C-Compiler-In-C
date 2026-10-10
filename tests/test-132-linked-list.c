// exit: 6
struct N { int v; struct N *next; };
int main() { struct N a, b, c; a.v = 1; b.v = 2; c.v = 3; a.next = &b; b.next = &c; c.next = 0; int s = 0; struct N *p = &a; while (p) { s += p->v; p = p->next; } return s; }
