// exit: 41
int sq(int x) { return x * x; }
int apply(int (*f)(int), int x) { return f(x) + f(x + 1); }
int main() { return apply(sq, 4); }
