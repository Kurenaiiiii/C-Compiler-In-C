// exit: 50
int sq(int x) { return x * x; }
int cube(int x) { return x * x * x; }
int apply(int (*f)(int), int x) { return f(x); }
int main() { int s = 0; for (int i = 1; i <= 3; i++) s += apply(sq, i) + apply(cube, i); return s; }
