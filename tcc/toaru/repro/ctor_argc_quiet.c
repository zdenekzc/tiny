/* constructor + main(argc) without printf: is stdio to blame, or just the reading
 * of the parameters of main after the constructor? This program prints nothing and
 * returns 0. The host does not pass the return code through $? (README §5), the
 * marker following it is what gets checked. */

static int ran;

__attribute__((constructor)) static void ctor(void) { ran = 1; }

int main(int argc, char **argv) {
    (void)argv;
    return argc > 0 ? 0 : 1;
}
