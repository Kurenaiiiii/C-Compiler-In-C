// exit: 9
void set(int *p) { *p = 9; }
int main() { int x = 0; set(&x); return x; }
