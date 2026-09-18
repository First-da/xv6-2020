#include "kernel/types.h"
#include "kernel/param.h"
#include "user/user.h"

int main(int argc, char **argv)
{
    char buf[512];
    char *cmd[MAXARG];
    char c;
    int n = 0;
    int base;

    if(argc < 2){
        fprintf(2, "Usage: xargs command [args...]\n");
        exit(1);
    }

    base = argc - 1;

    for(int i = 1; i < argc; i++)
        cmd[i - 1] = argv[i];

    while(read(0, &c, 1) == 1){
        if(c != '\n'){
            if(n >= sizeof(buf) - 1){
                fprintf(2, "xargs: line too long\n");
                exit(1);
            }

            buf[n++] = c;
            continue;
        }

        buf[n] = 0;

        int index = base;
        char *p = buf;

        while(*p){
            while(*p == ' ' || *p == '\t')
                p++;

            if(*p == 0)
                break;

            if(index >= MAXARG - 1){
                fprintf(2, "xargs: too many arguments\n");
                exit(1);
            }

            cmd[index++] = p;

            while(*p != 0 && *p != ' ' && *p != '\t')
                p++;

            if(*p != 0){
                *p = 0;
                p++;
            }
        }

        cmd[index] = 0;

        if(fork() == 0){
            exec(cmd[0], cmd);
            fprintf(2, "xargs: exec failed\n");
            exit(1);
        }

        wait(0);
        n = 0;
    }

    if(n > 0){
        buf[n] = 0;

        int index = base;
        char *p = buf;

        while(*p){
            while(*p == ' ' || *p == '\t')
                p++;

            if(*p == 0)
                break;

            if(index >= MAXARG - 1){
                fprintf(2, "xargs: too many arguments\n");
                exit(1);
            }

            cmd[index++] = p;

            while(*p != 0 && *p != ' ' && *p != '\t')
                p++;

            if(*p != 0){
                *p = 0;
                p++;
            }
        }

        cmd[index] = 0;

        if(fork() == 0){
            exec(cmd[0], cmd);
            fprintf(2, "xargs: exec failed\n");
            exit(1);
        }

        wait(0);
    }

    exit(0);
}