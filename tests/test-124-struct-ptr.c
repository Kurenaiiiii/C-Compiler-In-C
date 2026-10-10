// exit: 10
struct P { int x; int y; };
int main() { struct P p; struct P *q = &p; q->x = 10; q->y = 20; return p.x - q->y + 20; }
