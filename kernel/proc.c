#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"

#ifdef COW
extern int page_refs[];
extern struct spinlock page_refs_lock;
#endif

extern uint64 sys_uptime(void);
struct cpu cpus[NCPU];

struct proc proc[NPROC];

struct proc *mulq[5][NPROC];

int size_q[5] = {0};
struct proc *initproc;

int nextpid = 1;
struct spinlock pid_lock;

extern void forkret(void);
static void freeproc(struct proc *p);

extern char trampoline[]; // trampoline.S

// helps ensure that wakeups of wait()ing
// parents are not lost. helps obey the
// memory model when using p->parent.
// must be acquired before any p->lock.
struct spinlock wait_lock;

// Allocate a page for each process's kernel stack.
// Map it high in memory, followed by an invalid
// guard page.
void
proc_mapstacks(pagetable_t kpgtbl)
{
  struct proc *p;
  
  for(p = proc; p < &proc[NPROC]; p++) {
    char *pa = kalloc();
    if(pa == 0)
      panic("kalloc");
    uint64 va = KSTACK((int) (p - proc));
    kvmmap(kpgtbl, va, (uint64)pa, PGSIZE, PTE_R | PTE_W);
  }
}

int
kdo_rand(unsigned long *ctx)
{
/*
 * Compute x = (7^5 * x) mod (2^31 - 1)
 * without overflowing 31 bits:
 *      (2^31 - 1) = 127773 * (7^5) + 2836
 * From "Random number generators: good ones are hard to find",
 * Park and Miller, Communications of the ACM, vol. 31, no. 10,
 * October 1988, p. 1195.
 */
    long hi, lo, x;

    /* Transform to [1, 0x7ffffffe] range. */
    x = (*ctx % 0x7ffffffe) + 1;
    hi = x / 127773;
    lo = x % 127773;
    x = 16807 * lo - 2836 * hi;
    if (x < 0)
        x += 0x7fffffff;
    /* Transform to [0, 0x7ffffffd] range. */
    x--;
    *ctx = x;
    return (x);
}

unsigned long rand_next = 1;

int
krand(void)
{
    return (kdo_rand(&rand_next));
}

int min(int a, int b)
{
  if(a < b)
    return a;
  else
    return b;
}

int max(int a, int b)
{
  if(a > b)
    return a;
  else
    return b;
}

int enqueue(struct proc *p, int level)
{
  if(p->state != RUNNABLE)
		return -1;
	for(int i = 0; i<size_q[level]; i++)
		if(mulq[level][i]->pid == p->pid)
			return -1;

	// Otherwise we put it at the back of the queue
	p->curr_ticks = 0;
	p->queue_number = level;
	mulq[level][size_q[level]++] = p;

	return 0;
}

int dequeue(struct proc *p, int level)
{
  if(size_q[level] == 0)
    return -1;
  for(int i = 0; i < size_q[level];i++)
  {
    if(mulq[level][i]->pid == p->pid)
    {
      mulq[level][i] = 0;
      for(int j = i; j<size_q[level]-1; j++)
			{
				mulq[level][j] = mulq[level][j+1];
			}
			size_q[level] -= 1;
			return 0;
    }
  }
  return -1;
}

// initialize the proc table.
void
procinit(void)
{
  struct proc *p;
  
  initlock(&pid_lock, "nextpid");
  initlock(&wait_lock, "wait_lock");
  for(p = proc; p < &proc[NPROC]; p++) {
      initlock(&p->lock, "proc");
      p->state = UNUSED;
      p->kstack = KSTACK((int) (p - proc));
  }
}

// Must be called with interrupts disabled,
// to prevent race with process being moved
// to a different CPU.
int
cpuid()
{
  int id = r_tp();
  return id;
}

// Return this CPU's cpu struct.
// Interrupts must be disabled.
struct cpu*
mycpu(void)
{
  int id = cpuid();
  struct cpu *c = &cpus[id];
  return c;
}

// Return the current struct proc *, or zero if none.
struct proc*
myproc(void)
{
  push_off();
  struct cpu *c = mycpu();
  struct proc *p = c->proc;
  pop_off();
  return p;
}

int
allocpid()
{
  int pid;
  
  acquire(&pid_lock);
  pid = nextpid;
  nextpid = nextpid + 1;
  release(&pid_lock);

  return pid;
}

// Look in the process table for an UNUSED proc.
// If found, initialize state required to run in the kernel,
// and return with p->lock held.
// If there are no free procs, or a memory allocation fails, return 0.
static struct proc*
allocproc(void)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    if(p->state == UNUSED) {
      goto found;
    } else {
      release(&p->lock);
    }
  }
  return 0;

found:
  p->pid = allocpid();
  p->state = USED;

  // Allocate a trapframe page.
  if((p->trapframe = (struct trapframe *)kalloc()) == 0){
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // An empty user page table.
  p->pagetable = proc_pagetable(p);
  if(p->pagetable == 0){
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // Set up new context to start executing at forkret,
  // which returns to user space.
  memset(&p->context, 0, sizeof(p->context));
  p->context.ra = (uint64)forkret;
  p->context.sp = p->kstack + PGSIZE;
  p->rtime = 0;
  p->etime = 0;
  p->ctime = ticks;
  // Setting proc to be untraced at the start
  p->trace_mask = 0;
  p->interval = 0;
  p->fnptr = (uint64)-1;
  p->time_passed = 0;
  p->in_handler = 0;
  p->creation_time = sys_uptime();
  p->tickets = 1;
  p->prioirity = 60;
  p->running_time = 0;
  p->sleeping_time = 0;
  p->scheduled = 0;
  p->last_scheduled = sys_uptime();
  p->curr_ticks = 0;
  p->queue_number = 0;
  p->rtime = 0;
  p->etime = 0;
  p->ctime = ticks;
  // printf("%d\n", p->pid);
  // printf("%p\n", p->creation_time);
  // printf("%d\n", p->pid);
  return p;
}

// free a proc structure and the data hanging from it,
// including user pages.
// p->lock must be held.
static void
freeproc(struct proc *p)
{
  if(p->trapframe)
    kfree((void*)p->trapframe);
  p->trapframe = 0;
  if(p->pagetable)
    proc_freepagetable(p->pagetable, p->sz);
  p->pagetable = 0;
  p->sz = 0;
  p->pid = 0;
  p->parent = 0;
  p->name[0] = 0;
  p->chan = 0;
  p->killed = 0;
  p->xstate = 0;
  p->trace_mask = 0;
  p->interval = 0;
  p->fnptr = (uint64)-1;
  p->time_passed = 0;
  p->state = UNUSED;
}

// Create a user page table for a given process, with no user memory,
// but with trampoline and trapframe pages.
pagetable_t
proc_pagetable(struct proc *p)
{
  pagetable_t pagetable;

  // An empty page table.
  pagetable = uvmcreate();
  if(pagetable == 0)
    return 0;

  // map the trampoline code (for system call return)
  // at the highest user virtual address.
  // only the supervisor uses it, on the way
  // to/from user space, so not PTE_U.
  if(mappages(pagetable, TRAMPOLINE, PGSIZE,
              (uint64)trampoline, PTE_R | PTE_X) < 0){
    uvmfree(pagetable, 0);
    return 0;
  }

  // map the trapframe page just below the trampoline page, for
  // trampoline.S.
  if(mappages(pagetable, TRAPFRAME, PGSIZE,
              (uint64)(p->trapframe), PTE_R | PTE_W) < 0){
    uvmunmap(pagetable, TRAMPOLINE, 1, 0);
    uvmfree(pagetable, 0);
    return 0;
  }

  return pagetable;
}

// Free a process's page table, and free the
// physical memory it refers to.
void
proc_freepagetable(pagetable_t pagetable, uint64 sz)
{
  uvmunmap(pagetable, TRAMPOLINE, 1, 0);
  uvmunmap(pagetable, TRAPFRAME, 1, 0);
  uvmfree(pagetable, sz);
}

// a user program that calls exec("/init")
// assembled from ../user/initcode.S
// od -t xC ../user/initcode
uchar initcode[] = {
  0x17, 0x05, 0x00, 0x00, 0x13, 0x05, 0x45, 0x02,
  0x97, 0x05, 0x00, 0x00, 0x93, 0x85, 0x35, 0x02,
  0x93, 0x08, 0x70, 0x00, 0x73, 0x00, 0x00, 0x00,
  0x93, 0x08, 0x20, 0x00, 0x73, 0x00, 0x00, 0x00,
  0xef, 0xf0, 0x9f, 0xff, 0x2f, 0x69, 0x6e, 0x69,
  0x74, 0x00, 0x00, 0x24, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00
};

// Set up first user process.
void
userinit(void)
{
  struct proc *p;

  p = allocproc();
  initproc = p;
  
  // allocate one user page and copy initcode's instructions
  // and data into it.
  uvmfirst(p->pagetable, initcode, sizeof(initcode));
  p->sz = PGSIZE;

  // prepare for the very first "return" from kernel to user.
  p->trapframe->epc = 0;      // user program counter
  p->trapframe->sp = PGSIZE;  // user stack pointer

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");

  p->state = RUNNABLE;

  #ifdef MLFQ
  enqueue(p, 0);
  #endif
  release(&p->lock);
}

// Grow or shrink user memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n)
{
  uint64 sz;
  struct proc *p = myproc();

  sz = p->sz;
  if(n > 0){
    if((sz = uvmalloc(p->pagetable, sz, sz + n, PTE_W)) == 0) {
      return -1;
    }
  } else if(n < 0){
    sz = uvmdealloc(p->pagetable, sz, sz + n);
  }
  p->sz = sz;
  return 0;
}

int cowfork(){
    return 0;
}


// Create a new process, copying the parent.
// Sets up child kernel stack to return as if from fork() system call.
#ifndef COW
int
fork(void)
{
  int i, pid;
  struct proc *np;
  struct proc *p = myproc();

  // Allocate process.
  if((np = allocproc()) == 0){
    return -1;
  }

  // Copy user memory from parent to child.
  if(uvmcopy(p->pagetable, np->pagetable, p->sz) < 0){
    freeproc(np);
    release(&np->lock);
    return -1;
  }
  np->sz = p->sz;
  np->trace_mask = p->trace_mask; // child of tracee is also a tracee
  np->tickets = p->tickets;
  // printf("%d\n", np->tickets);
  // copy saved user registers.
  *(np->trapframe) = *(p->trapframe);

  // Cause fork to return 0 in the child.
  np->trapframe->a0 = 0;

  // increment reference counts on open file descriptors.
  for(i = 0; i < NOFILE; i++)
    if(p->ofile[i])
      np->ofile[i] = filedup(p->ofile[i]);
  np->cwd = idup(p->cwd);

  safestrcpy(np->name, p->name, sizeof(p->name));

  pid = np->pid;

  release(&np->lock);

  acquire(&wait_lock);
  np->parent = p;
  release(&wait_lock);

  acquire(&np->lock);
  np->state = RUNNABLE;
  #ifdef MLFQ
  enqueue(np, 0);
  np->queue_number = 0;
  np->curr_ticks = 0;
  np->last_scheduled = sys_uptime(); 
  #endif
  release(&np->lock);

  return pid;
}
#endif
#ifdef COW

int
fork(void)
{
  // return fork();
  int i, pid;
  struct proc *np;
  struct proc *p = myproc();

  // Allocate process.
  if((np = allocproc()) == 0){
    return -1;
  }
  int sz = p->sz;
  for(int i = 0; i < sz; i += PGSIZE){
    
    
    pte_t* pte = walk(p->pagetable, i, 0);
    if(pte == 0)
      panic("cow: pte should exist");
    if((*pte & PTE_V) == 0)
      panic("cow: page not present");
    uint64 pa = PTE2PA(*pte);
    //  if(!pa){
    //   continue;
    //  }
    acquire(&page_refs_lock);
    int page_num = ((uint64)pa / PGSIZE);
    page_refs[page_num]++;
    // if(page_num == 0)
        // printf("mum mum: %d %d\n", page_num, page_refs[page_num]);
    release(&page_refs_lock);
    // printf("flags: %d, pte2: %d\n", PTE_FLAGS(*pte), *pte);
    int flags = (PTE_FLAGS(*pte)) & (~PTE_W);
    *pte = PA2PTE(pa) | flags;
    // TODO: check if PTE_X is there or not before putting flag
    mappages(np->pagetable, i, PGSIZE, pa, flags);
  }
  
  np->sz = p->sz;
  np->trace_mask = p->trace_mask; // child of tracee is also a tracee

  // copy saved user registers.
  *(np->trapframe) = *(p->trapframe);

  // Cause fork to return 0 in the child.
  np->trapframe->a0 = 0;

  // increment reference counts on open file descriptors.
  for(i = 0; i < NOFILE; i++)
    if(p->ofile[i])
      np->ofile[i] = filedup(p->ofile[i]);
  np->cwd = idup(p->cwd);

  safestrcpy(np->name, p->name, sizeof(p->name));

  pid = np->pid;

  release(&np->lock);
  // printf("HELLO\n");
  acquire(&wait_lock);
  np->parent = p;
  release(&wait_lock);

  acquire(&np->lock);
  np->state = RUNNABLE;
  release(&np->lock);

  return pid;
}
#endif
// Pass p's abandoned children to init.
// Caller must hold wait_lock.
void
reparent(struct proc *p)
{
  struct proc *pp;

  for(pp = proc; pp < &proc[NPROC]; pp++){
    if(pp->parent == p){
      pp->parent = initproc;
      wakeup(initproc);
    }
  }
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait().
void
exit(int status)
{
  struct proc *p = myproc();

  if(p == initproc)
    panic("init exiting");

  // Close all open files.
  for(int fd = 0; fd < NOFILE; fd++){
    if(p->ofile[fd]){
      struct file *f = p->ofile[fd];
      fileclose(f);
      p->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(p->cwd);
  end_op();
  p->cwd = 0;

  acquire(&wait_lock);

  // Give any children to init.
  reparent(p);

  // Parent might be sleeping in wait().
  wakeup(p->parent);
  
  acquire(&p->lock);

  p->xstate = status;
  p->state = ZOMBIE;
  p->etime = ticks;

  release(&wait_lock);

  // printf("EXITED\n");
  // Jump into the scheduler, never to return.
  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
wait(uint64 addr)
{
  struct proc *pp;
  int havekids, pid;
  struct proc *p = myproc();

  acquire(&wait_lock);

  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(pp = proc; pp < &proc[NPROC]; pp++){
      if(pp->parent == p){
        // make sure the child isn't still in exit() or swtch().
        acquire(&pp->lock);

        havekids = 1;
        if(pp->state == ZOMBIE){
          // Found one.
          pid = pp->pid;
          if(addr != 0 && copyout(p->pagetable, addr, (char *)&pp->xstate,
                                  sizeof(pp->xstate)) < 0) {
            release(&pp->lock);
            release(&wait_lock);
            return -1;
          }
          freeproc(pp);
          release(&pp->lock);
          release(&wait_lock);
          return pid;
        }
        release(&pp->lock);
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || killed(p)){
      release(&wait_lock);
      return -1;
    }
    #ifdef MLFQ
    dequeue(p, p->queue_number);
    #endif
    
    // Wait for a child to exit.
    sleep(p, &wait_lock);  //DOC: wait-sleep
  }
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
waitx(uint64 addr, uint* wtime, uint* rtime)
{
  struct proc *np;
  int havekids, pid;
  struct proc *p = myproc();

  acquire(&wait_lock);

  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(np = proc; np < &proc[NPROC]; np++){
      if(np->parent == p){
        // make sure the child isn't still in exit() or swtch().
        acquire(&np->lock);

        havekids = 1;
        if(np->state == ZOMBIE){
          // Found one.
          pid = np->pid;
          *rtime = np->rtime;
          *wtime = np->etime - np->ctime - np->rtime;
          if(addr != 0 && copyout(p->pagetable, addr, (char *)&np->xstate,
                                  sizeof(np->xstate)) < 0) {
            release(&np->lock);
            release(&wait_lock);
            return -1;
          }
          freeproc(np);
          release(&np->lock);
          release(&wait_lock);
          return pid;
        }
        release(&np->lock);
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || p->killed){
      release(&wait_lock);
      return -1;
    }

    // Wait for a child to exit.
    sleep(p, &wait_lock);  //DOC: wait-sleep
  }
}

void
update_time()
{
  struct proc* p;
  for (p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    if (p->state == RUNNING) {
      p->rtime++;
    }
    release(&p->lock); 
  }
}



// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run.
//  - swtch to start running that process.
//  - eventually that process transfers control
//    via swtch back to the scheduler.


#ifndef FCFS
#ifndef LBS
#ifndef PBS
#ifndef MLFQ
void
scheduler(void)
{
  // printf("SWITCHING...\n");

  struct proc *p;
  struct cpu *c = mycpu();
  
  c->proc = 0;
  for(;;){
    // Avoid deadlock by ensuring that devices can interrupt.
    intr_on();

    for(p = proc; p < &proc[NPROC]; p++) {
      acquire(&p->lock);
      if(p->state == RUNNABLE) {
        // Switch to chosen process.  It is the process's job
        // to release its lock and then reacquire it
        // before jumping back to us.
        p->state = RUNNING;
        c->proc = p;
        swtch(&c->context, &p->context);

        // Process is done running for now.
        // It should have changed its p->state before coming back.
        c->proc = 0;
      }
      release(&p->lock);
    }
  }
}
#endif
#endif
#endif
#endif

#ifdef FCFS
void
scheduler(void)
{
  // printf("SWITCHING...\n");

  struct proc *p;
  struct cpu *c = mycpu();
  
  c->proc = 0;
  for(;;){
    // Avoid deadlock by ensuring that devices can interrupt.
    intr_on();
    struct proc* this_proc = 0;
    for(p = proc; p < &proc[NPROC]; p++) {
      acquire(&p->lock);
      if(p->state == RUNNABLE) {
        // printf("p->state\n");
        // Switch to chosen process.  It is the process's job
        // to release its lock and then reacquire it
        // before jumping back to us.
        if(this_proc == 0)
        {
          // acquire(&this_proc->lock);
          this_proc = p;
          // release(&this_proc->lock);
        }
        else
        {
          if(this_proc->creation_time > p->creation_time)
          {
            // acquire(&this_proc->lock);
            // struct proc *temp = this_proc;
            this_proc = p;
            // release(&temp->lock);
          }
        }
      }
      release(&p->lock);
    }
    if(this_proc != 0)
    {
      acquire(&this_proc->lock);
      if(this_proc->state == RUNNABLE)
      {
        this_proc->state = RUNNING;
        c->proc = this_proc;
        swtch(&c->context, &this_proc->context);
        c->proc = 0;
      }
      release(&this_proc->lock);
    }

    // Process is done running for now.
    // It should have changed its p->state before coming back.
  }
}
#endif

#ifdef LBS
void
scheduler(void)
{
  // printf("SWITCHING...\n");

  struct proc *p;
  struct cpu *c = mycpu();
  
  c->proc = 0;
  for(;;){
    // printf("SCHEDULING\n");
    int sum = 0;
    // Avoid deadlock by ensuring that devices can interrupt.
    intr_on();

    for(p = proc; p < &proc[NPROC]; p++) {
      acquire(&p->lock);
      if(p->state == RUNNABLE) {
        // Switch to chosen process.  It is the process's job
        // to release its lock and then reacquire it
        // before jumping back to us.

        sum += p->tickets;
        // Process is done running for now.
        // It should have changed its p->state before coming back.
      }
      release(&p->lock);
    }
    struct proc *this_proc = 0;
    int new_sum = 0;
    int random_number = krand()%sum;
    for(p = proc; p < &proc[NPROC]; p++) {
       acquire(&p->lock);
      if(p->state == RUNNABLE) {
        // Switch to chosen process.  It is the process's job
        // to release its lock and then reacquire it
        // before jumping back to us.
        new_sum += p->tickets;
        if(random_number < new_sum)
        {
          this_proc = p;
          release(&p->lock);
          break;
        }
        // Process is done running for now.
        // It should have changed its p->state before coming back.
      }
      release(&p->lock);
    }
    if(this_proc != 0)
    {
      acquire(&this_proc->lock);
      if(this_proc->state == RUNNABLE)
      {  
        this_proc->state = RUNNING;
        c->proc = this_proc;
        swtch(&c->context, &this_proc->context);
        c->proc = 0;
      }
      release(&this_proc->lock);
    }
  }
}
#endif


#ifdef PBS
void
scheduler(void)
{
  // printf("SWITCHING...\n");

  struct proc *p;
  struct cpu *c = mycpu();

  c->proc = 0;
  for(;;){
    // Avoid deadlock by ensuring that devices can interrupt.
    intr_on();
    struct proc *max_pr = 0;
    int min_dp = 100;
    for(p = proc; p < &proc[NPROC]; p++) {
      acquire(&p->lock);
      if(p->state == RUNNABLE) {
        if(max_pr == 0)
        {
          max_pr = p;
          int dp = 100;
          if(p->running_time == 0 && p->sleeping_time == 0)
          {
            dp = max(0, min(p->prioirity, 100));
          }
          else
          {
            int niceness = (int)(((int)p->sleeping_time)*10/(p->sleeping_time+p->running_time));
            dp = max(0, min(p->prioirity - niceness + 5, 100));
          }
          // printf("%d\n", dp);
          min_dp = dp;
        }
        else
        { 
          int dp = 100;
          if(p->running_time == 0 && p->sleeping_time == 0)
          {
            dp = max(0, min(p->prioirity, 100));
          }
          else
          {
            int niceness = (int)(((int)p->sleeping_time)*10/(p->sleeping_time+p->running_time));
            dp = max(0, min(p->prioirity - niceness + 5, 100));
          }
          // printf("%d\n", dp);
          if(dp < min_dp)
          {
            max_pr = p;
            min_dp = dp;
          }
          else if(dp == min_dp)
          {
            if(p->scheduled < max_pr->scheduled)
            {
              max_pr = p;
            }
            else if(p->scheduled == max_pr->scheduled)
            {
              if(p->creation_time < max_pr->creation_time)
              {
                max_pr = p;
              }
            }
          }
        }
      }
      release(&p->lock);
    }
    if(max_pr != 0)
    {
      // printf("%d %d %d\n", min_dp, max_pr->sleeping_time, max_pr->running_time);
      acquire(&max_pr->lock);
      if(max_pr->state == RUNNABLE)
      { 
        max_pr->state = RUNNING;
        max_pr->running_time = 0;
        max_pr->sleeping_time = 0;
        max_pr->scheduled++;
        c->proc = max_pr;
        swtch(&c->context, &max_pr->context);
        c->proc = 0;
      }
      release(&max_pr->lock);
    }
  }
}
#endif

#ifdef MLFQ

int check_exceed(struct proc *p)
{
  // printf(">>pid: %d, queue: %d, currticks: %d\n", p->pid, p->queue_number, p->curr_ticks);
  if(p->queue_number == 0 && p->curr_ticks >= 1)
    return 1;
  if(p->queue_number == 1 && p->curr_ticks >= 2)
    return 1;
  if(p->queue_number == 2 && p->curr_ticks >= 4)
    return 1;
  if(p->queue_number == 3 && p->curr_ticks >= 8)
    return 1;
  if(p->queue_number == 4 && p->curr_ticks >= 16)
    return 1;
  return 0;
}

int check_higher(struct proc *p)
{
  for(int level = 0 ; level < p->queue_number ; level++)
  {
    for(int i=0;i < size_q[level];i++)
    {
      if(mulq[level][i]->state == RUNNABLE)
        return 1;
    }
  }
  return 0;
}

void
scheduler(void)
{
  struct cpu *c = mycpu();
  c->proc = 0;
  // printf("SWITCHING...\n");
  for(;;){
    intr_on();
    for(struct proc *p = proc; p < &proc[NPROC]; p++)
      if(p->state == RUNNABLE)
        enqueue(p, p->queue_number);
    for(int level = 1;level<5;level++)
    {
      for(int i=0;i<size_q[level];i++)
      {
        struct proc *pr = mulq[level][i];
        acquire(&pr->lock);
        int age = sys_uptime() - pr->last_scheduled;
        if(age > MAX_AGE)
        {
          dequeue(pr, level);
          pr->last_scheduled = sys_uptime();
          pr->queue_number = level-1;
          pr->curr_ticks = 0;
          enqueue(pr, level - 1);
        }
        release(&pr->lock);
      }
    }

    struct proc* this_proc = 0;
    for(int level=0;level<5;level++)
    {
      if(size_q[level] > 0)
      {
        this_proc = mulq[level][0];
        dequeue(this_proc, level);
        break;
      }
    }
    if(this_proc != 0)
    {
      acquire(&this_proc->lock);
      if(this_proc-> state == RUNNABLE)
      {
        this_proc->last_scheduled = sys_uptime();
        this_proc->state = RUNNING;
        c->proc = this_proc;
        swtch(&c->context, &this_proc->context);
        c->proc = 0;

      }
      release(&this_proc->lock);
    }
  }
}

#endif
// Switch to scheduler.  Must hold only p->lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->noff, but that would
// break in the few places where a lock is held but
// there's no process.
void
sched(void)
{
  // printf("SWITCHING\n");
  int intena;
  struct proc *p = myproc();

  if(!holding(&p->lock))
    panic("sched p->lock");
  if(mycpu()->noff != 1)
    panic("sched locks");
  if(p->state == RUNNING)
    panic("sched running");
  if(intr_get())
    panic("sched interruptible");

  intena = mycpu()->intena;
  swtch(&p->context, &mycpu()->context);
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void
yield(void)
{ 
  // printf("YIELDING\n");
  struct proc *p = myproc();
  acquire(&p->lock);
  p->state = RUNNABLE;
  // printf("SCHED\n");
  sched();
  release(&p->lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch to forkret.
void
forkret(void)
{
  static int first = 1;

  // Still holding p->lock from scheduler.
  release(&myproc()->lock);

  if (first) {
    // File system initialization must be run in the context of a
    // regular process (e.g., because it calls sleep), and thus cannot
    // be run from main().
    first = 0;
    fsinit(ROOTDEV);
  }

  usertrapret();
}

// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
void
sleep(void *chan, struct spinlock *lk)
{
  struct proc *p = myproc();
  // printf("%d\n", p->pid);
  // Must acquire p->lock in order to
  // change p->state and then call sched.
  // Once we hold p->lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup locks p->lock),
  // so it's okay to release lk.

  acquire(&p->lock);  //DOC: sleeplock1
  release(lk);

  // Go to sleep.
  p->chan = chan;
  p->state = SLEEPING;
  // printf("SCHED111\n");
  sched();

  // Tidy up.
  p->chan = 0;

  // Reacquire original lock.
  release(&p->lock);
  acquire(lk);
}

// Wake up all processes sleeping on chan.
// Must be called without any p->lock.
void
wakeup(void *chan)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++) {
    if(p != myproc()){
      acquire(&p->lock);
      if(p->state == SLEEPING && p->chan == chan) {
        p->state = RUNNABLE;
      }
      release(&p->lock);
    }
  }
}

// Kill the process with the given pid.
// The victim won't exit until it tries to return
// to user space (see usertrap() in trap.c).
int
kill(int pid)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++){
    acquire(&p->lock);
    if(p->pid == pid){
      p->killed = 1;
      if(p->state == SLEEPING){
        // Wake process from sleep().
        p->state = RUNNABLE;
      }
      release(&p->lock);
      return 0;
    }
    release(&p->lock);
  }
  return -1;
}

void
setkilled(struct proc *p)
{
  acquire(&p->lock);
  p->killed = 1;
  release(&p->lock);
}

int
killed(struct proc *p)
{
  int k;
  
  acquire(&p->lock);
  k = p->killed;
  release(&p->lock);
  return k;
}

// Copy to either a user address, or kernel address,
// depending on usr_dst.
// Returns 0 on success, -1 on error.
int
either_copyout(int user_dst, uint64 dst, void *src, uint64 len)
{
  struct proc *p = myproc();
  if(user_dst){
    return copyout(p->pagetable, dst, src, len);
  } else {
    memmove((char *)dst, src, len);
    return 0;
  }
}

// Copy from either a user address, or kernel address,
// depending on usr_src.
// Returns 0 on success, -1 on error.
int
either_copyin(void *dst, int user_src, uint64 src, uint64 len)
{
  struct proc *p = myproc();
  if(user_src){
    return copyin(p->pagetable, dst, src, len);
  } else {
    memmove(dst, (char*)src, len);
    return 0;
  }
}

// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void
procdump(void)
{
  static char *states[] = {
  [UNUSED]    "unused",
  [USED]      "used",
  [SLEEPING]  "sleep ",
  [RUNNABLE]  "runable",
  [RUNNING]   "run   ",
  [ZOMBIE]    "zombie"
  };
  struct proc *p;
  char *state;

  printf("\n[");
  for(p = proc; p < &proc[NPROC]; p++){
    if(p->state == UNUSED || p->pid <= 2) // TODO
      continue;
    if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    printf("{\"pid\": %d, \"state\": %s,  ", p->pid, state);
    #ifdef PBS
      printf("prno: %d, sleepy: %d, runny: %d", p->prioirity, p->sleeping_time, p->running_time);
    #endif
    #ifdef FCFS
    printf(" %d %d", p->creation_time, p->rtime);
    #endif
    #ifdef MLFQ
    printf("\"qno\": %d, \"curr\": %d, \"lstschd\": %d, \"tot\": %d },", p->queue_number, p->curr_ticks, p->last_scheduled, sys_uptime() - p->creation_time);
    #endif
    #ifdef LBS
    printf("\"tickets\": %d},", p->tickets);
    #endif
    printf("\n");
  }
  printf("],\n");
}

int 
trace(int pid, int trace_mask){
  struct proc *p;
  int success = -1;
  for(p = proc; p < &proc[NPROC]; p++){
    if(p->pid == pid){
      p->trace_mask = trace_mask;
      success = 0;
    }
  }
  return success;
}

int
sigalarm(int interval, uint64 fnptr){
  struct proc *p;
  p = myproc();
  if(p!=0){
    p->interval = interval;
    p->fnptr = fnptr;
    return 0;
  }
  else
    return 1;
}

int
sigreturn(){
  struct proc* p = myproc();
  memmove(p->trapframe, p->extra_frame, PGSIZE);
  kfree(p->extra_frame);
  p->in_handler = 0;
  usertrapret();
  return 0;
}

int
settickets(int tickets)
{
  struct proc *p = myproc();
  p->tickets = tickets;
  return 0;
}

void update_info()
{
  struct proc *p;
  for(p = proc; p < &proc[NPROC]; p++) {
      acquire(&p->lock);
      if(p->state == SLEEPING)
      {
        p->sleeping_time++;
        // if(p->pid > 2)
        //   printf("SLEEPING %d\n", p->sleeping_time);
      }
      else if(p->state == RUNNING)
      {
        p->running_time++;
        // printf("RUNNING\n");
      }
      release(&p->lock);
    }
}

int setpriority(int priority, int pid)
{
  struct proc *p;
  int returnVal = -1;
  for(p = proc ; p < &proc[NPROC] ; p++)
  {
    acquire(&p->lock);
    // printf("%d\n", p->pid);
    if(p->pid == pid)
    {
      returnVal = p->prioirity;
      p->prioirity = priority;
      p->running_time = 0;
      p->sleeping_time = 0;
    }
    release(&p->lock);
  }
  // printf("%d %d\n", returnVal, priority);
  if(returnVal > priority)
  {
    yield();
  }
  return returnVal;
}