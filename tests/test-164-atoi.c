// exit: 42
int atoi(char *s) { int n = 0; int neg = 0; if (*s == '-') { neg = 1; s++; } while (*s >= '0' && *s <= '9') { n = n * 10 + (*s - '0'); s++; } return neg ? -n : n; }
int main() { return atoi("42"); }
