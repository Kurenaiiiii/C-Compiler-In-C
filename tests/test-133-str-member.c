// exit: 147
struct S { char name[8]; int id; };
int main() { struct S s; s.name[0] = 'A'; s.name[1] = 0; s.id = 82; return s.id + s.name[0]; }
