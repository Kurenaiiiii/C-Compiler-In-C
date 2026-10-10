// exit: 9
struct P { int x; };
int main() { struct P a[3]; a[0].x = 1; a[1].x = 2; a[2].x = 9; return a[2].x; }
