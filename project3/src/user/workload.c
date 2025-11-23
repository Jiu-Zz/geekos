/**********************************************************************

        Project 2: Multiprogramming


  By:
      Jeff Hollingsworth
  e-mail:
      hollings@cs.umd.edu

  File:        proc2.c
  Created on:  3/6/1996
  Contents:    User Processes to be run concurrently
      1.    Init() : Initial process, creates other ones & terminates
      2..4  Long() , Ping() , and Pong() are three examplar processes
      that are to be run concurently.  Long() is a CPU intensive
          job, while Ping() and Pong() bounce back between one another.
      5.    Wait() : busy wait for a key to be pressed

  Simon Hawkin <cema@cs.umd.edu> 03/16/1998
      - Added progress monitoring output in Long().
  Jeff Hollingsworth <hollings@cs.umd.edu> 2/19/02
      - Re-written for the new project
  David Hovemeyer <daveho@cs.umd.edu> 2/27/04
      - Update for GeekOS 0.2.0


  [2] An enclosed test ("proc2.c", or "encl2.c").

**********************************************************************/

#include <conio.h>
#include <process.h>
#include <sched.h>
#include <sema.h>
#include <string.h>

#define MAX_SHORT_PROCESSES 5
#define START_SEM_NAME "start_gate"

#if !defined(NULL)
#define NULL 0
#endif

int main(int argc, char **argv)
{
    int i;
    int policy = -1;
    int start;
    int elapsed;
    int quantum;
    int scr_sem;       /* sid of screen semaphore */
    int start_sem;
    int id_long; /* ID of child process */
    int id_short[MAX_SHORT_PROCESSES];

    if (argc == 3)
    {
        if (!strcmp(argv[1], "rr"))
        {
            policy = 0;
        }
        else if (!strcmp(argv[1], "mlf"))
        {
            policy = 1;
        }
        else
        {
            Print("usage: %s [rr|mlf] <quantum>\n", argv[0]);
            Exit(1);
        }
        quantum = atoi(argv[2]);
        Set_Scheduling_Policy(policy, quantum);
    }
    else
    {
        Print("usage: %s [rr|mlf] <quantum>\n", argv[0]);
        Exit(1);
    }

    start = Get_Time_Of_Day();
    Print("Test started at %d\n", start);
    scr_sem = Create_Semaphore("screen", 1);
    start_sem = Create_Semaphore(START_SEM_NAME, 0);

    P(scr_sem);
    Print("************* Start Workload Generator *********\n");
    V(scr_sem);

    id_long = Spawn_Program("/c/long.exe", "/c/long.exe");
    P(scr_sem);
    Print("\nLong = %d ", id_long);
    V(scr_sem);

    for (i = 0; i < MAX_SHORT_PROCESSES; i++)
    {
        id_short[i] = Spawn_Program("/c/short.exe", "/c/short.exe");
        P(scr_sem);
        Print("\nShort %d = %d ", i+1, id_short[i]);
        V(scr_sem);
    }

    P(scr_sem);
    Print("\nStart all processes\n");
    V(scr_sem);

    for (i = 0; i < 1 + MAX_SHORT_PROCESSES; ++i)
    {
        V(start_sem);
    }

    Wait(id_long);
    for(i = 0; i < MAX_SHORT_PROCESSES; i++)
    {
        Wait(id_short[i]);
    }

    elapsed = Get_Time_Of_Day() - start;
    Print("\nTests Completed at %d\n", elapsed);
    return 0;
}

