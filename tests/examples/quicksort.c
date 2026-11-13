#include <stdio.h>
#include <stdlib.h> /* srand, rand */
#include <time.h>   /* time */

const int N = 1000;
// const int N = 1000000;

int a [N];

inline void swap (int & a, int & b)
{
    int t = a;
    a = b;
    b = t;
}

void quicksort (int r, int s)
{
    int h = a [(r + s) / 2];    // r <= (r+s)/2 <= s
    int i = r;
    int k = s;

    while (i <= k)
    {
        while (a[i] < h) i++;
        while (a[k] > h) k--;

        if (i <= k)
        {
            swap (a[i], a[k]);
            i++;
            k--;
        }
    }

    if (r < k) quicksort (r, k);
    if (i < s) quicksort (i, s);
}

int main()
{
    srand (time (NULL));

    for (int i = 0; i < N; i++)
        a[i] = rand() % 50000;

    quicksort (0, N-1);

    bool ok = true;
    for (int i = 0; i < N; i++)
    {
        printf ("%d", a[i]);
        if (i < N - 1 && a[i] > a[i + 1]) { ok = false; printf (" WRONG"); }
        printf ("\n");
    }
    if (ok)  printf ("O.K.\n");
}
