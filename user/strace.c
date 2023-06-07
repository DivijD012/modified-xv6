#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

//This function will call trace on the process with pid with bitmask set to trace_mask

int strace(int pid, int trace_mask)
{
    trace(pid, trace_mask);
return 0;
}

// accepts the bitmask and the command as input
// forks a new process and execs
int main(int argc, char *argv[])
{
    int trace_mask = atoi(argv[1]);
    int pid = fork();
    if(pid > 0)
    {
        strace(pid, trace_mask);
    }
    if(pid == 0)
    {
        if(exec(argv[2], &argv[2]) == -1)
        {
            printf("Panicked while tracing");
            return -1;
        }
    }
    wait(0);
    return 0;
}