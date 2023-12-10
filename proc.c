#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "x86.h"
#include "proc.h"
#include "spinlock.h"

struct {
  struct spinlock lock;
  struct proc proc[NPROC];
} ptable;

static struct proc *initproc;

int nextpid = 1;
int nextLCFS = 1;
extern void forkret(void);
extern void trapret(void);

static void wakeup1(void *chan);

void
pinit(void)
{
  initlock(&ptable.lock, "ptable");
}

// Must be called with interrupts disabled
int
cpuid() {
  return mycpu()-cpus;
}

// Must be called with interrupts disabled to avoid the caller being
// rescheduled between reading lapicid and running through the loop.
struct cpu*
mycpu(void)
{
  int apicid, i;
  
  if(readeflags()&FL_IF)
    panic("mycpu called with interrupts enabled\n");
  
  apicid = lapicid();
  // APIC IDs are not guaranteed to be contiguous. Maybe we should have
  // a reverse map, or reserve a register to store &cpus[i].
  for (i = 0; i < ncpu; ++i) {
    if (cpus[i].apicid == apicid)
      return &cpus[i];
  }
  panic("unknown apicid\n");
}

// Disable interrupts so that we are not rescheduled
// while reading proc from the cpu structure
struct proc*
myproc(void) {
  struct cpu *c;
  struct proc *p;
  pushcli();
  c = mycpu();
  p = c->proc;
  popcli();
  return p;
}

//PAGEBREAK: 32
// Look in the process table for an UNUSED proc.
// If found, change state to EMBRYO and initialize
// state required to run in the kernel.
// Otherwise return 0.
static struct proc*
allocproc(void)
{
  struct proc *p;
  char *sp;

  acquire(&ptable.lock);

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == UNUSED)
      goto found;

  release(&ptable.lock);
  return 0;

found:
  p->state = EMBRYO;
  p->pid = nextpid++;

  p->queue = LCFS;                            // default queue: LCFS ***************************************
  p->lcsf_order = nextLCFS++;
  p->creation_time = ticks;
  p->waited_start_time = p->creation_time;
  p->executed_cycle = 0;

  acquire(&tickslock);
  p->priority = (ticks * ticks * 1021) % 100;   // generates a pseudorandom priority 
  release(&tickslock);

  p->priority_ratio = 0.2;
  p->executed_cycle_ratio = 0.2;
  // p->creation_time_ratio = 0.2;
  p->arrival_time_ratio = 0.2;
  p->process_size_ratio = 1;

  release(&ptable.lock);

  // Allocate kernel stack.
  if((p->kstack = kalloc()) == 0){
    p->state = UNUSED;
    return 0;
  }
  sp = p->kstack + KSTACKSIZE;

  // Leave room for trap frame.
  sp -= sizeof *p->tf;
  p->tf = (struct trapframe*)sp;

  // Set up new context to start executing at forkret,
  // which returns to trapret.
  sp -= 4;
  *(uint*)sp = (uint)trapret;

  sp -= sizeof *p->context;
  p->context = (struct context*)sp;
  memset(p->context, 0, sizeof *p->context);
  p->context->eip = (uint)forkret;

  return p;
}

//PAGEBREAK: 32
// Set up first user process.
void
userinit(void)
{
  struct proc *p;
  extern char _binary_initcode_start[], _binary_initcode_size[];

  p = allocproc();
  
  initproc = p;
  if((p->pgdir = setupkvm()) == 0)
    panic("userinit: out of memory?");
  inituvm(p->pgdir, _binary_initcode_start, (int)_binary_initcode_size);
  p->sz = PGSIZE;
  memset(p->tf, 0, sizeof(*p->tf));
  p->tf->cs = (SEG_UCODE << 3) | DPL_USER;
  p->tf->ds = (SEG_UDATA << 3) | DPL_USER;
  p->tf->es = p->tf->ds;
  p->tf->ss = p->tf->ds;
  p->tf->eflags = FL_IF;
  p->tf->esp = PGSIZE;
  p->tf->eip = 0;  // beginning of initcode.S

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");

  // this assignment to p->state lets other cores
  // run this process. the acquire forces the above
  // writes to be visible, and the lock is also needed
  // because the assignment might not be atomic.
  acquire(&ptable.lock);

  p->state = RUNNABLE;

  release(&ptable.lock);
}

// Grow current process's memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n)
{
  uint sz;
  struct proc *curproc = myproc();

  sz = curproc->sz;
  if(n > 0){
    if((sz = allocuvm(curproc->pgdir, sz, sz + n)) == 0)
      return -1;
  } else if(n < 0){
    if((sz = deallocuvm(curproc->pgdir, sz, sz + n)) == 0)
      return -1;
  }
  curproc->sz = sz;
  switchuvm(curproc);
  return 0;
}

// Create a new process copying p as the parent.
// Sets up stack to return as if from system call.
// Caller must set state of returned proc to RUNNABLE.
int
fork(void)
{
  int i, pid;
  struct proc *np;
  struct proc *curproc = myproc();

  // Allocate process.
  if((np = allocproc()) == 0){
    return -1;
  }

  // Copy process state from proc.
  if((np->pgdir = copyuvm(curproc->pgdir, curproc->sz)) == 0){
    kfree(np->kstack);
    np->kstack = 0;
    np->state = UNUSED;
    return -1;
  }
  np->sz = curproc->sz;
  np->parent = curproc;
  *np->tf = *curproc->tf;

  // Clear %eax so that fork returns 0 in the child.
  np->tf->eax = 0;

  for(i = 0; i < NOFILE; i++)
    if(curproc->ofile[i])
      np->ofile[i] = filedup(curproc->ofile[i]);
  np->cwd = idup(curproc->cwd);

  safestrcpy(np->name, curproc->name, sizeof(curproc->name));

  pid = np->pid;

  acquire(&ptable.lock);

  np->state = RUNNABLE;

  release(&ptable.lock);

  return pid;
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait() to find out it exited.
void
exit(void)
{
  struct proc *curproc = myproc();
  struct proc *p;
  int fd;

  if(curproc == initproc)
    panic("init exiting");

  // Close all open files.
  for(fd = 0; fd < NOFILE; fd++){
    if(curproc->ofile[fd]){
      fileclose(curproc->ofile[fd]);
      curproc->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(curproc->cwd);
  end_op();
  curproc->cwd = 0;

  acquire(&ptable.lock);

  // Parent might be sleeping in wait().
  wakeup1(curproc->parent);

  // Pass abandoned children to init.
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->parent == curproc){
      p->parent = initproc;
      if(p->state == ZOMBIE)
        wakeup1(initproc);
    }
  }

  // Jump into the scheduler, never to return.
  curproc->state = ZOMBIE;
  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
wait(void)
{
  struct proc *p;
  int havekids, pid;
  struct proc *curproc = myproc();
  
  acquire(&ptable.lock);
  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->parent != curproc)
        continue;
      havekids = 1;
      if(p->state == ZOMBIE){
        // Found one.
        pid = p->pid;
        kfree(p->kstack);
        p->kstack = 0;
        freevm(p->pgdir);
        p->pid = 0;
        p->parent = 0;
        p->name[0] = 0;
        p->killed = 0;
        p->state = UNUSED;
        release(&ptable.lock);
        return pid;
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || curproc->killed){
      release(&ptable.lock);
      return -1;
    }

    // Wait for children to exit.  (See wakeup1 call in proc_exit.)
    sleep(curproc, &ptable.lock);  //DOC: wait-sleep
  }
}

//**************************************************************************************************************
// BJF scheduler

int calculate_rank(struct proc* p){
  return ((p->priority * p->priority_ratio) +
          (p->creation_time * p->arrival_time_ratio) +
          (p->executed_cycle * p->executed_cycle_ratio) + (p->sz * p->process_size_ratio));
}

struct proc* best_job_first(void)
{
  struct proc* p;
  struct proc* best_proc = 0;
  float min_rank = 999999999;
  float rank;

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){ 
    if (p->state != RUNNABLE)
      continue;
    else if (p->queue != 3) 
      continue;
    
    rank =  calculate_rank(p);

    if (rank < min_rank){
      best_proc = p;
      min_rank = rank;
    }
  }

  return best_proc;
}

void aging(void)
{
  struct proc* p = 0;

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++) {

    // p->waited_cycles = ticks - creation_tie;

    // if (p->state != RUNNABLE || p->queue == 1)
    //   continue;

    if (p->queue == 1)
      continue;


    if (ticks - p->creation_time > 1000) {
      p->queue = 1;
      p->waited_start_time = 0;
    }
  }
}

struct proc* LCFSSched(void){
  struct proc *p;
  int target_pid = -1;
  int stack_top = -1;
  
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->state != RUNNABLE || p->queue != LCFS)
      continue;
    if(p->lcsf_order > stack_top){
      target_pid = p->pid;
    }
  }
  
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->state != RUNNABLE || p->queue != LCFS)
      continue;
    
    if(p->pid == target_pid)
      return p;
    }

  return 0;
}

// Round Robin schedular

struct proc* round_robin(void)
{
  struct proc *p;
  struct proc *best_proc = 0;


  for (p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if (p->state != RUNNABLE || p->queue != ROUNDROBIN)
        continue;

      return p;
  }
  return best_proc;
}

void 
scheduler(void)
{
  struct proc *p = 0;
  struct cpu *c = mycpu();
  c->proc = 0;

  for (;;)
  {
    // Enable interrupts on this processor.
    sti();

    // Loop over process table looking for process to run.
    acquire(&ptable.lock);
    p = round_robin();
    if (p == 0) {
      p = LCFSSched();
    }
    if (p == 0) {
      p = best_job_first();
    }
    if (p == 0) {
      release(&ptable.lock);
      continue;
    }
    // cprintf("%d" , p->waited_cycles);
    aging();
    

    // Switch to chosen process.  It is the process's job
    // to release ptable.lock and then reacquire it
    // before jumping back to us.
    c->proc = p;
    switchuvm(p);
    p->state = RUNNING;
    p->waited_start_time = 0;

    swtch(&(c->scheduler), p->context);
    switchkvm();

    // Process is done running for now.
    // It should have changed its p->state before coming back.
    c->proc = 0;

    release(&ptable.lock);
  }
}

// Enter scheduler.  Must hold only ptable.lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->ncli, but that would
// break in the few places where a lock is held but
// there's no process.
void
sched(void)
{
  int intena;
  struct proc *p = myproc();

  if(!holding(&ptable.lock))
    panic("sched ptable.lock");
  if(mycpu()->ncli != 1)
    panic("sched locks");
  if(p->state == RUNNING)
    panic("sched running");
  if(readeflags()&FL_IF)
    panic("sched interruptible");
  intena = mycpu()->intena;
  swtch(&p->context, mycpu()->scheduler);
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void //********************************************************************************************************
 yield(void)
{
  acquire(&ptable.lock); // DOC: yieldlock
  myproc()->state = RUNNABLE;
  myproc()->executed_cycle += 0.1;
  myproc()->last_executed_time = ticks;
  sched();
  release(&ptable.lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch here.  "Return" to user space.
void
forkret(void)
{
  static int first = 1;
  // Still holding ptable.lock from scheduler.
  release(&ptable.lock);

  if (first) {
    // Some initialization functions must be run in the context
    // of a regular process (e.g., they call sleep), and thus cannot
    // be run from main().
    first = 0;
    iinit(ROOTDEV);
    initlog(ROOTDEV);
  }

  // Return to "caller", actually trapret (see allocproc).
}

// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
void
sleep(void *chan, struct spinlock *lk)
{
  struct proc *p = myproc();
  
  if(p == 0)
    panic("sleep");

  if(lk == 0)
    panic("sleep without lk");

  // Must acquire ptable.lock in order to
  // change p->state and then call sched.
  // Once we hold ptable.lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup runs with ptable.lock locked),
  // so it's okay to release lk.
  if(lk != &ptable.lock){  //DOC: sleeplock0
    acquire(&ptable.lock);  //DOC: sleeplock1
    release(lk);
  }
  // Go to sleep.
  p->chan = chan;
  p->state = SLEEPING;

  sched();

  // Tidy up.
  p->chan = 0;

  // Reacquire original lock.
  if(lk != &ptable.lock){  //DOC: sleeplock2
    release(&ptable.lock);
    acquire(lk);
  }
}

//PAGEBREAK!
// Wake up all processes sleeping on chan.
// The ptable lock must be held.
static void
wakeup1(void *chan)
{
  struct proc *p;

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == SLEEPING && p->chan == chan)
      p->state = RUNNABLE;
}

// Wake up all processes sleeping on chan.
void
wakeup(void *chan)
{
  acquire(&ptable.lock);
  wakeup1(chan);
  release(&ptable.lock);
}

// Kill the process with the given pid.
// Process won't exit until it returns
// to user space (see trap in trap.c).
int
kill(int pid)
{
  struct proc *p;

  acquire(&ptable.lock);
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->pid == pid){
      p->killed = 1;
      // Wake process from sleep if necessary.
      if(p->state == SLEEPING)
        p->state = RUNNABLE;
      release(&ptable.lock);
      return 0;
    }
  }
  release(&ptable.lock);
  return -1;
}

//PAGEBREAK: 36
// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void
procdump(void)
{
  static char *states[] = {
  [UNUSED]    "unused",
  [EMBRYO]    "embryo",
  [SLEEPING]  "sleep ",
  [RUNNABLE]  "runble",
  [RUNNING]   "run   ",
  [ZOMBIE]    "zombie"
  };
  int i;
  struct proc *p;
  char *state;
  uint pc[10];

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->state == UNUSED)
      continue;
    if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    cprintf("%d %s %s", p->pid, state, p->name);
    if(p->state == SLEEPING){
      getcallerpcs((uint*)p->context->ebp+2, pc);
      for(i=0; i<10 && pc[i] != 0; i++)
        cprintf(" %p", pc[i]);
    }
    cprintf("\n");
  }
}

//*************************************************************************************************************
struct calling_process
{
  int pids[1000];
  int size;
};

struct calling_process call_ps[26] = {0};

void push_callerp(int pid, int num)
{
  int this_size = call_ps[num].size;
  call_ps[num].size++;
  call_ps[num].pids[this_size] = pid;
}

void get_callers(int sys_call_number)
{
  int size_of_callers = call_ps[sys_call_number].size;
  for (int i = 0; i < size_of_callers-1; i++)
  {
    cprintf("%d, ", call_ps[sys_call_number].pids[i]);
  }
  cprintf("%d", call_ps[sys_call_number].pids[size_of_callers-1]);
}

char* get_state_name(int state)
{
  if (state == 0)
    return "UNUSED";

  else if (state == 1) 
    return "EMBRYO";
  
  else if (state == 2) 
    return "SLEEPING";
  
  else if (state == 3) 
    return "RUNNABLE";
  
  else if (state == 4) 
    return "RUNNING";
  
  else if (state == 5) 
    return "ZOMBIE";
  
  else 
    return "";
}

char* get_queue_name(int level)
{
  if (level == 1)
    return "RoundRobin";

  else if (level == 2)
    return "LCFS";

  else if (level == 3)
    return "BJF"; 

  else
    return "Undefined";
}

int get_num_len(int number)
{
  int len = 0;

  if (number == 0)
    return 1;

  while (number > 0){
    len++;
    number = number / 10;
  }
  return len;
}


void print_all_procs_status()
{
  struct proc *p;
  cprintf("\n");
  cprintf("Name");
  for (int i = 0 ; i < 15 - strlen("name") ; i++)
    cprintf(" ");

  cprintf("Pid");
  for (int i = 0 ; i < 10 - strlen("pid") ; i++)
    cprintf(" ");

  cprintf("State");
  for (int i = 0 ; i < 15 - strlen("state") ; i++)
    cprintf(" ");

  cprintf("Queue_level");
  for (int i = 0 ; i < 17 - strlen("queue_level") ; i++)
    cprintf(" ");

  cprintf("Arrival");
  for (int i = 0 ; i < 12 - strlen("arrival") ; i++)
    cprintf(" ");

  cprintf("BJF_Rank");
  for (int i = 0 ; i < 12 - strlen("bjf_Rank") ; i++)
    cprintf(" ");

  cprintf("p-e-a-s ratio*100");
  for (int i = 0 ; i < 20 - strlen("p-e-a-s ratio*100") ; i++)
    cprintf(" ");

  cprintf("executed_cycles*10");
  for (int i = 0 ; i < 20 - strlen("executed_cycles*10") ; i++)
    cprintf(" ");

  cprintf("\n");
  for (int i = 0 ; i < 140 ; i++)
    cprintf("-");
  cprintf("\n");

  acquire(&ptable.lock);                                                  // Lock procs table to get current information (in order to stop procs table changing)
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){ 
    if (p->state == UNUSED)
      continue;

    cprintf(p->name);
    for (int i = 0 ; i < 15 - strlen(p->name) ; i++)
      cprintf(" ");

    cprintf("%d", p->pid);
    for (int i = 0 ; i < 10 - get_num_len(p->pid) ; i++)
      cprintf(" ");

    cprintf(get_state_name(p->state));
    for (int i = 0 ; i < 15 - strlen(get_state_name(p->state)) ; i++)
      cprintf(" "); 

    cprintf(get_queue_name(p->queue));
    for (int i = 0 ; i < 17 - strlen(get_queue_name(p->queue)) ; i++)
      cprintf(" "); 

    cprintf("%d" , p->creation_time);
    for (int i = 0 ; i < 17 - get_num_len(p->creation_time) ; i++)
      cprintf(" ");

    cprintf("%d" , calculate_rank(p));
    for (int i = 0 ; i < 15 - get_num_len(calculate_rank(p)) ; i++)
      cprintf(" ");

    int pRate = p->priority_ratio * 100;
    int eRate = p->executed_cycle_ratio * 100;
    int aRate = p->arrival_time_ratio * 100;
    int sRate = p->process_size_ratio * 100;


    cprintf("%d %d %d %d" , pRate , eRate , aRate , sRate);
    for (int i = 0 ; i < 17 - (get_num_len(pRate) + get_num_len(eRate) + get_num_len(aRate) + get_num_len(sRate)) ; i++)
      cprintf(" ");

    cprintf("%d" , p->executed_cycle * 10);
    for (int i = 0 ; i < 15 - get_num_len(p->executed_cycle) ; i++)
      cprintf(" ");

    cprintf("\n");
  }
  cprintf("\n");
  release(&ptable.lock);

}

void set_proc_queue(int pid, int queue_level)
{
  struct proc *p;

  acquire(&ptable.lock);
  for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if (p->pid == pid){
      p->queue = queue_level;
      p->waited_start_time = 0;
    }
  release(&ptable.lock);
}

void set_bjf_params(int pid, int priority_ratio, int arrival_time_ratio, int executed_cycle_ratio,int process_size_ratio)
{
  struct proc *p;

  acquire(&ptable.lock);
  for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
  {
    if (p->pid == pid)
    {
      p->priority_ratio = priority_ratio;
      p->arrival_time_ratio = arrival_time_ratio;
      p->executed_cycle_ratio = executed_cycle_ratio;
      p->process_size_ratio = process_size_ratio;
       
    }
  }
  release(&ptable.lock); 
}

void set_all_bjf_params(int priority_ratio, int arrival_time_ratio, int executed_cycle_ratio,int process_size_ratio)
{
  struct proc *p;

  acquire(&ptable.lock);
  for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
  {
      p->priority_ratio = priority_ratio;
      p->arrival_time_ratio = arrival_time_ratio;
      p->executed_cycle_ratio = executed_cycle_ratio; 
      p->process_size_ratio = process_size_ratio;
  }
  release(&ptable.lock); 
}
//*****************************************************************************************************************
