#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

void trivial(); 
int main(int argc, char **argv)
{    
    trivial();
    // int a = 10;
    // int pid = fork();
    // printf("AAAAAAAAAA%d\n", pid);
    // if(pid == 0){
    //     // settickets()
    //     sleep(200);
    //     // a = 20;
    //     printf("%d %d\n", pid, a);
    //     exit(0);
    // }
    // else{
    //     sleep(200);
    //     wait(0);
    //     // for(int i = 0; i < 1000000000; i++){
    //     //     asm volatile("nop");
    //     // }
    //     a = 30;
    //     printf("%d %d\n", pid, a);
       
    // }
    int ti = 10;
    for(int i = 0; i < 4; i++){
        if(fork() == 0){
            settickets(ti);
            int loops = 5;
            while(loops--)
                for (volatile int i = 0; i < 1000000000; i++) {
                asm volatile("nop");
                }
            exit(0);
        }
        ti += 10;
    }
    wait(0);
    return 0;
}

void trivial()
{
    printf("THIS IS TRIVIAL\n");
    // sigreturn();
}