// exit: 3
int count(char *s, char c) { int n = 0; while (*s) { if (*s == c) n++; s++; } return n; }
int main() { return count("banana", 'a'); }
