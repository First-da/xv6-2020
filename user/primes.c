#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int myfork(int *arr)
{
    close(arr[1]);
    int first;
    int tmp;
    if (read(arr[0], &first, sizeof(int)) != sizeof(int)) {
            return -1;
    } else {
        fprintf(2, "prime %d\n", first);
    }

    int p[2];
    pipe(p);
    int pid = fork();
    if (pid == 0) {
        myfork(p);
    } else {
        while(read(arr[0], &tmp, sizeof(int)) == sizeof(int)) {
            if (tmp % first != 0) {
                write(p[1], &tmp, sizeof(int));
            }
        }
        close(p[1]);

        close(arr[0]);
            wait(0);
    }
    return 0;
}

int main(int argc, char **argv)
{
    int p[2];
    pipe(p);

    for (int i = 2; i <= 35; i++) {
        write(p[1], &i, sizeof(int));
    }

    int state = myfork(p);
    if (state < 0) {
        fprintf(2, "excute err!\n");
        exit(1);
    }

    exit(0);
}