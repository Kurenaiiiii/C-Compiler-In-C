// exit: 9
int main() { int x = 9; int *p = &x; int **pp = &p; return **pp; }
