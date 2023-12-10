// init: The initial user-level program

#include "types.h"
#include "stat.h"
#include "user.h"
#include "fcntl.h"

char *argv[] = { "sh", 0 };

int
main(void)
{
  int pid, wpid;

  if(open("console", O_RDWR) < 0){
    mknod("console", 1, 1);
    open("console", O_RDWR);
  }
  dup(0);  // stdout
  dup(0);  // stderr

  for(;;){
    printf(1, "init: starting sh\n");
    printf(1 , "Make multipe process to test : foo&\n");
    printf(1 , "Set a bjf params : set_bjf_params [PID] [P_ratio] [A_ratio] [E_ratio] [SZ]\n");
    printf(1 , "Set all bjf params : set_all_bjf_params [P_ratio] [A_ratio] [E_ratio] [SZ]\n");
    printf(1 , "Change a queue : set_queue [PID] [Destination(1 , 2 , 3)]\n");
    printf(1 , "List all process scheduling inforamtion : procs_status\n");
    pid = fork();
    if(pid < 0){
      printf(1, "init: fork failed\n");
      exit();
    }
    if(pid == 0){
      exec("sh", argv);
      printf(1, "init: exec sh failed\n");
      exit();
    }
    while((wpid=wait()) >= 0 && wpid != pid)
      printf(1, "zombie!\n");
  }
}
