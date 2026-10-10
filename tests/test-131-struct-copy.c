// exit: 11
struct P { int x; int y; };
int main() { struct P p1; p1.x = 5; p1.y = 6; struct P p2; p2 = p1; return p2.x + p2.y; }
