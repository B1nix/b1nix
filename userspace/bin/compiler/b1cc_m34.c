/* On-target (x86_64-b1nix) proof that b1cc's M34 feature set compiles and runs
 * inside the b1nix kernel. Pure computation (no libc dependency); returns 0
 * only if every feature behaves correctly. Spawned by kernel/user/programs.c,
 * which reports B1CC-M34-SMOKE based on the exit status. */

/* K&R old-style definition */
int knr_add(a, b) int a; int b; { return a + b; }

/* wide string literal -> wchar_t (4-byte) array in .rodata */
static const int *wide = (const int *)L"Hi";   /* {72, 105, 0} */

struct P { long a, b, c; };

int main(void) {
    /* wide string literal element values (4-byte wchar_t) */
    if (wide[0] != 72 || wide[1] != 105 || wide[2] != 0) return 1;

    /* K&R function */
    if (knr_add(40, 2) != 42) return 2;

    /* VLA with a run-time bound */
    int n = 4;
    int vla[n];
    for (int i = 0; i < n; i++) vla[i] = i * i;   /* 0,1,4,9 */
    if (vla[3] != 9) return 3;

    /* designated + partial aggregate initializer (omitted -> zero) */
    int arr[6] = { [0] = 5, [5] = 7 };
    if (arr[0] != 5 || arr[1] != 0 || arr[4] != 0 || arr[5] != 7) return 4;

    /* local partial struct init zeroes the rest */
    struct P p = { 11 };
    if (p.a != 11 || p.b != 0 || p.c != 0) return 5;

    /* _Complex as 16-byte storage */
    double _Complex z;
    if (sizeof(z) != 16) return 6;

    return 0;
}
