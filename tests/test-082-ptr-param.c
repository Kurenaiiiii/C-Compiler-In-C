// exit: 42
void inc(int *p) { (*p)++; }
int main() { int x = 41; inc(&x); return x; }
