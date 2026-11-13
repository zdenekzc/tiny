#include <stdio.h>
#include <assert.h>

#define N 5 // number of disks

typedef struct Tower
{
    int v;      // tower height
    int p [N];  // disk sizes, p[0] is at the bottom, p[v-1] is the top
} Tower;

Tower A, B, C;

void clear (Tower * X)
{
   X->v = 0; // zero height

   for (int i = 0; i<N; i++)
        X->p [i] = 0; // for debugging - zero everything
}

void init (Tower * X)
{
    X->v = N; // N disks
    for (int i = 0; i<N; i++)
        X->p [i] = N-i; // bottom p[0] = N, ..., top p[N-1] = 1
}

void show (Tower * X)
{
    for (int i = 0; i < X->v; i++)
        printf ("%d", X->p[i]);
    for (int i = X->v; i <= N; i++)
        printf (" "); // pad with spaces
}

void print_towers (Tower * X, Tower * Y, Tower * Z)
{
    show (X);
    printf (" : ");
    show (Y);
    printf (" : ");
    show (Z);
    printf ("\n");
}

void move_disk (Tower * X, Tower * Y) // move a disk from tower X to tower Y
{
    int h;
    assert (X->v > 0); // check that the tower is not empty

    h = X->p [X->v-1]; // disk number from the top of tower X
    X->p [X->v-1] = 0; // for debugging - zero it
    X->v = X->v - 1;  // decrease the height of tower X

    assert (Y->v < N); // check that the tower is not too high
    if (Y->v > 0)
    {
        assert (h < Y->p [Y->v-1]); // whether h is smaller than the current top
    }

    Y->v = Y->v + 1; // increase tower Y
    Y->p [Y->v-1] = h; // add disk h on top
}

void solve (Tower * X, Tower * Y, Tower * Z, int k)
// move from tower X to tower Z, tower Y will be the auxiliary tower
// we move k disks from the top of tower X
{
    if (k > 1) solve (X, Z, Y, k-1); // k-1 disks, X --> Y, Z is the auxiliary tower
    move_disk (X, Z); // 1 disk, X --> Z
    print_towers (&A, &B, &C); // print one step
    if (k > 1) solve (Y, X, Z, k-1); // k-1 disks, Y --> Z, X is the auxiliary tower
}

int main (int argc, char * argv [])
{
    init (&A); // tower A - N disks
    clear (&B); // towers B and C are empty
    clear (&C);

    print_towers (&A, &B, &C); // initial state
    solve (&A, &B, &C, N); // A --> C, B is the auxiliary tower, we move N disks

    printf ("O.K.\n");
    return 0;
}
