#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int main(int argc, char **argv)
{
    int p[2];
    int c[2];
    if (argc != 1) {
        fprintf(2, "Usage: pingpong\n");
        exit(1);
    }

    if (pipe(p) < 0) {
        fprintf(2, "pipe failed!\n");
        exit(1);
    }

    if (pipe(c) < 0) {
        fprintf(2, "pipe failed!\n");
        exit(1);
    }

    int pid;
    if ((pid = fork()) == 0) {
        //child process
        char child;
        close(p[1]);
        close(c[0]);
        if (read(p[0], (char *)&child, 1) == 1) {
            fprintf(2, "%d: received ping\n", getpid());
        } else {
            fprintf(2, "child read failed\n");
            exit(1);
        }

        write(c[1], &child, 1);
        close(p[0]);            //close read
        close(c[1]);            //close write
    } else {
        char init = 'a';
        char parent;
        close(p[0]);
        close(c[1]);
        write(p[1], &init, 1);

        if (read(c[0], (char *)&parent, 1) == 1) {
            fprintf(2, "%d: received pong\n", getpid());
        } else {
            fprintf(2, "parent read failed\n");
            exit(1);
        }
        close(p[1]);            //close write
        close(c[0]);            //close read
    }
    
    exit(0);
}