// exit: 7
typedef int (*bin)(int, int);
int add(int a, int b) { return a + b; }
int main() { bin f = add; return f(3, 4); }
