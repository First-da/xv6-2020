#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/fs.h"
#include "kernel/fcntl.h"

int composite_name(char *argv1, char *argv2, char *buf)
{
    char output[512];
    char *p;
    if (strlen(argv1) + 1 + strlen(argv2) + 1 > sizeof(output)) {
        printf("too long path\n");
        return -1;
    }
    strcpy(output, argv1);
    p = output + strlen(output);
    *p = '/';
    p++;
    strcpy(p, argv2);
    strcpy(buf, output);
    return 0;
}

int myfind(char *path, char *target)
{
    struct dirent de;
    struct stat st;
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        fprintf(2, "input file err\n");
        exit(1);
    }

    if(fstat(fd, &st) < 0) {
        printf("file stat err!\n");
        exit(1);
    }

    switch (st.type)
    {
        case T_FILE:
        case T_DEVICE: {
            char file_name[512];
            char *p;
            memset(file_name, 0, sizeof(file_name));
            strcpy(file_name, path);
            p = file_name + strlen(file_name);
            while(p >= file_name && *p != '/') {
                p--;
            }
            p++;
            if (strcmp(p, target) == 0) {
                fprintf(1, "%s\n", path);
            }
            break;
        }
        
        case T_DIR:
            {
                char dir_buf[512];
                char name[DIRSIZ + 1];
                memset(dir_buf, 0, sizeof(dir_buf));
                while(read(fd, &de, sizeof(de)) == sizeof(de)) {

                    if (de.inum == 0) {
                        continue;
                    }
                    memmove(name, de.name, sizeof(de.name));
                    name[DIRSIZ] = '\0';
                    if ((strcmp(name, ".") == 0) || (strcmp(name, "..") == 0)) {
                        continue;
                    }
                    if (composite_name(path, name, dir_buf) < 0) {
                        continue;
                    }
                    

                    if ((strcmp(name, target) == 0) && (stat(dir_buf, &st) == 0) && (st.type == T_DIR)) {
                        fprintf(1, "%s\n", dir_buf);
                    }

                    myfind(dir_buf, target);
                }
                break;
            }
        default:
            break;
    }
    close(fd);
    return 0;
}


int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(2, "Usage: find <path> <name>");
        exit(1);
    }

    myfind(argv[1], argv[2]);

    exit(0);
}