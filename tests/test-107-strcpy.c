// exit: 245
int main() { char d[8]; char *s = "hi!"; int i = 0; while ((d[i] = s[i])) i++; return d[0] + d[1] + d[2] + i; }
