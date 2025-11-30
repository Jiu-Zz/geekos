/* ppload.c
 * Simple loader to start ping and pong programs to exercise semaphores.
 */

#include <conio.h>
#include <process.h>
#include <sched.h>
#include <sema.h>
#include <string.h>

int main(int argc, char **argv)
{
  int scr_sem;
  int ping, pong;
  int id1, id2;

  scr_sem = Create_Semaphore("screen", 1);

  ping = Create_Semaphore("ping", 1);

  pong = Create_Semaphore("pong", 0);

  P(scr_sem);
  Print("ppload: starting ping and pong\n");
  V(scr_sem);

  id1 = Spawn_Program("/c/ping.exe", "/c/ping.exe");
  id2 = Spawn_Program("/c/pong.exe", "/c/pong.exe");

  Wait(id1);
  Wait(id2);

  P(scr_sem);
  Print("ppload: both finished\n");
  V(scr_sem);

  Destroy_Semaphore(ping);
  Destroy_Semaphore(pong);
  Destroy_Semaphore(scr_sem);

  return 0;
}

