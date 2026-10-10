// exit: 15
struct N { int v; struct N *next; };
int main() { struct N n1, n2, n3, n4, n5; n1.v = 1; n2.v = 2; n3.v = 3; n4.v = 4; n5.v = 5; n1.next = &n2; n2.next = &n3; n3.next = &n4; n4.next = &n5; n5.next = 0; int s = 0; for (struct N *p = &n1; p; p = p->next) s += p->v; return s; }
