#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

// Write a program that uses UNIX system calls to ''ping-pong'' a byte between two
// processes over a pair of pipes, one for each direction. The parent should send a
// byte to the child; the child should print ": received ping", where is its process ID,
// write the byte on the pipe to the parent, and exit; the parent should read the byte
// from the child, print ": received pong", and exit. Your solution should be in the file
// user/pingpong.c .

int main(int argc, char *argv[])
{
    if (argc != 1)
    {
        fprintf(2, "usage: pingpong\n");
        exit(1);
    }

    int p[2];
    pipe(p);

    int pid = fork();
    if (pid == 0)
    {
        // 子进程
        char buf;
        while (read(p[0], &buf, 1) == 0)
            ;
        close(p[0]);
        fprintf(1, "%d: received ping\n", getpid());
        write(p[1], &buf, 1);
        close(p[1]);
        exit(0);
    }
    else
    {
        // 父进程
        char buf = 'x';
        write(p[1], &buf, 1);
        close(p[1]);
        // 等待子进程结束并回收
        wait(0);
        read(p[0], &buf, 1);
        close(p[0]);
        fprintf(1, "%d: received pong\n", getpid());
    }
    exit(0);
}