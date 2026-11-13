#include <stdio.h>
#include <stdlib.h>
#include <time.h>

const int N = 100;

int a[N];

inline void swap (int & x, int & y)
{
    int t = x;
    x = y;
    y = t;
}

void heapify(int i, int k)
// build a heap from a[i] ... a[k]
// assuming a[i+1] ... a[k] already form a heap
{
    while (2 * i + 1 <= k)
    {
        int v = 2 * i + 1;

        if (v + 1 <= k)
            if (a[v+1] > a[v])
                v = v + 1;

        if (a[i] < a[v])
        {
            swap (a[i], a[v]);
            i = v;
        }
        else
        {
            i = k+1; // terminate the loop
        }
    }
}

void heapsort (int n)
// sort a[0] ... a[n-1]
{
    for (int i = n-1; i >= 0; i--)
        heapify (i, n-1);

    for (int i = n-1; i > 0; i--) 
    {
        swap (a[0], a[i]);
        heapify (0, i-1);
    }
}

int main()
{
    srand (time (nullptr));
    for (int i = 0; i < N; i++)
        a[i] = rand() % 10000;

    heapsort (N);

    bool ok = true;
    for (int i = 0; i < N; i++)
    {
        printf ("%d", a[i]);
        if (i < N-1 && a[i + 1] < a[i]) { ok = false; printf (" WRONG"); }
        printf ("\n");
    }

    if (ok)
       printf ("O.K.\n"); 
}
