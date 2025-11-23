
#include "libuser.h"
#include "process.h"

int main(int argc, char **argv)
{
  int i, j ;     	/* loop index */
  int scr_sem;		/* id of screen semaphore */
  int start_sem;
  int now, start, elapsed;    		

  start_sem = Create_Semaphore("start_gate", 0);
  P(start_sem);

  start = Get_Time_Of_Day();
  scr_sem = Create_Semaphore ("screen" , 1) ;   /* register for screen use */

  for (i=0; i < 200; i++) {
      for (j=0 ; j < 1000 ; j++)
        now = Get_Time_Of_Day();
  }
  elapsed = Get_Time_Of_Day() - start;
  P (scr_sem) ;
  Print("\nProcess Long is done at time: %d\n", Get_Time_Of_Day()) ;
  V(scr_sem);


  return 0;
}


