#include <conio.h>
#include <process.h>
#include <sched.h>
#include <sema.h>
#include <string.h>

int main(int argc, char **argv)
{
    int i;
    int scr_sem;
    int start;
    int ping, pong;

    start = Get_Time_Of_Day();
    scr_sem = Create_Semaphore("screen", 1);
    ping = Create_Semaphore("ping", 1);
    pong = Create_Semaphore("pong", 0);

    for (i = 0; i < 5; i++)
    {
        P(ping);
        P(scr_sem);
        Print("ping\n");
        V(scr_sem);
        V(pong);
    }

    P(scr_sem);
    Print("Process Ping is done at time: %d\n", Get_Time_Of_Day() - start);
    V(scr_sem);

    return 0;
}

