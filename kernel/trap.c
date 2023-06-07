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
// #define FCFS

struct spinlock tickslock;
uint ticks;

extern char trampoline[], uservec[], userret[];


// in kernelvec.S, calls kerneltrap().
void kernelvec();

extern int devintr();

void
trapinit(void)
{
  initlock(&tickslock, "time");
}

// set up to take exceptions and traps while in the kernel.
void
trapinithart(void)
{
  w_stvec((uint64)kernelvec);
}

int check_alarm()
{ 
  
  struct proc *p = myproc();
  // printf("%d\n", p->fnptr);
  if(p->interval == 0)
    return 1;
  // if(p->fnptr == (uint64)-1)
  //   return 1;
  p->time_passed++;
  if(p->time_passed >= p->interval && p->in_handler == 0)
  { 
    p->in_handler = 1;
    p->time_passed = 0;
    struct trapframe* newframe = (struct trapframe*)kalloc();
    memmove(newframe, p->trapframe, PGSIZE);
    p->extra_frame = newframe;
    p->trapframe->epc = p->fnptr;
  }
  return 0;
}
#ifdef COW
int cow_handler(){
  // Plan of action:
  // Duplicate the page
  // Decrease reference from old
  // Make only the new one write mode


  // printf("PAGE FAULT\n");
  pte_t *pte;
  uint64 pa, i = r_stval();
  // printf("address referenced: %d\n", i);
  if(i >= MAXVA)
    return -1;
  if(i == 0)
    return -1;
  uint flags;
  char *mem;
  pagetable_t pg_table = myproc()->pagetable;
  // get the pte
  if((pte = walk(pg_table, i, 0)) == 0)
    return -1;
  
  

  // check if present
  if((*pte & PTE_V) == 0)
    return -1;

  // perms?
  if((*pte & PTE_U) == 0){
    return -1;
  }
  // physical address
  pa = PTE2PA(*pte);
  // if addr === 0 for some reason
  if(pa == 0)
    return -1;
  // decrease references
  // acquire(&page_refs_lock);
  // int page_num = ((uint64)pa / PGSIZE);
  // page_refs[page_num]--;
  // // if(num_refs == 0){
  // //   // kfree((void *)pa);
  // // }
  // // if(page_num == 0)
  //       printf("mum mum3: %d %d\n", page_num, page_refs[page_num]);
  // release(&page_refs_lock);
  // printf("%p\n", pa);
  // Add write perms
  flags = PTE_FLAGS((*pte)) | PTE_W;

  // duplicate page
  if((mem = kalloc()) == 0)
    return -1;
  memmove(mem, (char*)pa, PGSIZE);
  
  *pte =  PA2PTE(mem) | flags;
  // acquire(&page_refs_lock);
  // int num_refs = page_refs[page_num];
  // if(num_refs == 0){
  //   release(&page_refs_lock);
  //   kfree((void *)pa);
  // }
  // else
  //   release(&page_refs_lock);
  kfree((void *)pa);
  
  return 0;
}
#endif
//
// handle an interrupt, exception, or system call from user space.
// called from trampoline.S
//
void
usertrap(void)
{
  int which_dev = 0;

  if((r_sstatus() & SSTATUS_SPP) != 0)
    panic("usertrap: not from user mode");

  
  // send interrupts and exceptions to kerneltrap(),
  // since we're now in the kernel.
  w_stvec((uint64)kernelvec);

  struct proc *p = myproc();
  
  // save user program counter.
  p->trapframe->epc = r_sepc();
#ifdef COW
  if(r_scause() == 0xf ){ // STORE PAGE FAULT 
    if(r_stval() >= MAXVA){
      printf("usertrap()2: unexpected scause %p pid=%d\n", r_scause(), p->pid);
      printf("            sepc=%p stval=%p\n", r_sepc(), r_stval());
      setkilled(p);
    }
    else{
      if(cow_handler() == -1)
        setkilled(p);
    }
  }
  else 
#endif
    if(r_scause() == 8){
    // system call

    if(killed(p))
      exit(-1);

    // sepc points to the ecall instruction,
    // but we want to return to the next instruction.
    p->trapframe->epc += 4;

    // an interrupt will change sepc, scause, and sstatus,
    // so enable only now that we're done with those registers.
    intr_on();

    syscall();
  } else if((which_dev = devintr()) != 0){
    // ok
  } else {
    printf("usertrap(): unexpected scause %p pid=%d\n", r_scause(), p->pid);
    printf("            sepc=%p stval=%p\n", r_sepc(), r_stval());
    setkilled(p);
  }

  if(killed(p))
    exit(-1);

  // give up the CPU if this is a timer interrupt.
  if(which_dev == 2)
    {
      #ifdef MLFQ
      p->curr_ticks++;
      int val = check_exceed(p);
      // printf("val: %d, curr: %d, pid: %d\n", val, p->curr_ticks, p->pid);
      if(val == 1)
      {
        if(p->queue_number != 4)
          p->queue_number+=1;
        p->curr_ticks = 0;
        yield();
      }
      else
      {
        int val2 = check_higher(p);
        if(val2 == 1)
        {
          yield();
        }
      }
      #endif
      check_alarm();
      p->running_time++;
      #ifndef MLFQ
      #ifndef PBS
      #ifndef FCFS
      yield();
      #endif
      #endif
      #endif
    }

  usertrapret();
}

//
// return to user space
//
void
usertrapret(void)
{
  struct proc *p = myproc();

  // we're about to switch the destination of traps from
  // kerneltrap() to usertrap(), so turn off interrupts until
  // we're back in user space, where usertrap() is correct.
  intr_off();

  // send syscalls, interrupts, and exceptions to uservec in trampoline.S
  uint64 trampoline_uservec = TRAMPOLINE + (uservec - trampoline);
  w_stvec(trampoline_uservec);

  // set up trapframe values that uservec will need when
  // the process next traps into the kernel.
  p->trapframe->kernel_satp = r_satp();         // kernel page table
  p->trapframe->kernel_sp = p->kstack + PGSIZE; // process's kernel stack
  p->trapframe->kernel_trap = (uint64)usertrap;
  p->trapframe->kernel_hartid = r_tp();         // hartid for cpuid()

  // set up the registers that trampoline.S's sret will use
  // to get to user space.
  
  // set S Previous Privilege mode to User.
  unsigned long x = r_sstatus();
  x &= ~SSTATUS_SPP; // clear SPP to 0 for user mode
  x |= SSTATUS_SPIE; // enable interrupts in user mode
  w_sstatus(x);

  // set S Exception Program Counter to the saved user pc.
  w_sepc(p->trapframe->epc);

  // tell trampoline.S the user page table to switch to.
  uint64 satp = MAKE_SATP(p->pagetable);

  // jump to userret in trampoline.S at the top of memory, which 
  // switches to the user page table, restores user registers,
  // and switches to user mode with sret.
  uint64 trampoline_userret = TRAMPOLINE + (userret - trampoline);
  ((void (*)(uint64))trampoline_userret)(satp);
}

// interrupts and exceptions from kernel code go here via kernelvec,
// on whatever the current kernel stack is.
void 
kerneltrap()
{
  int which_dev = 0;
  uint64 sepc = r_sepc();
  uint64 sstatus = r_sstatus();
  uint64 scause = r_scause();

  if((sstatus & SSTATUS_SPP) == 0)
    panic("kerneltrap: not from supervisor mode");
  if(intr_get() != 0)
    panic("kerneltrap: interrupts enabled");

  if((which_dev = devintr()) == 0){
    printf("scause %p\n", scause);
    printf("sepc=%p stval=%p\n", r_sepc(), r_stval());
    panic("kerneltrap");
  }

  // give up the CPU if this is a timer interrupt.
  if(which_dev == 2 && myproc() != 0 && myproc()->state == RUNNING)
    {
      
      #ifdef MLFQ
      struct proc *p = myproc();
      p->curr_ticks++;
      int val = check_exceed(p);
      if(val == 1)
      {
        if(p->queue_number != 4)
          p->queue_number+=1;
        p->curr_ticks = 0;
        yield();
      }
      else
      {
        int val2 = check_higher(p);
        if(val2 == 1)
        {
          yield();
        }
      }
      #endif
      check_alarm();
      #ifndef MLFQ
      #ifndef PBS
      #ifndef FCFS
      yield();
      #endif
      #endif
      #endif
    }

  // the yield() may have caused some traps to occur,
  // so restore trap registers for use by kernelvec.S's sepc instruction.
  w_sepc(sepc);
  w_sstatus(sstatus);
}

void
clockintr()
{
  
  #ifdef PBS
  update_info();
  #endif
  update_time();
  acquire(&tickslock);
  ticks++;
  update_time();
  wakeup(&ticks);
  release(&tickslock);
}

// check if it's an external interrupt or software interrupt,
// and handle it.
// returns 2 if timer interrupt,
// 1 if other device,
// 0 if not recognized.
int
devintr()
{
  uint64 scause = r_scause();

  if((scause & 0x8000000000000000L) &&
     (scause & 0xff) == 9){
    // this is a supervisor external interrupt, via PLIC.

    // irq indicates which device interrupted.
    int irq = plic_claim();

    if(irq == UART0_IRQ){
      uartintr();
    } else if(irq == VIRTIO0_IRQ){
      virtio_disk_intr();
    } else if(irq){
      printf("unexpected interrupt irq=%d\n", irq);
    }

    // the PLIC allows each device to raise at most one
    // interrupt at a time; tell the PLIC the device is
    // now allowed to interrupt again.
    if(irq)
      plic_complete(irq);

    return 1;
  } else if(scause == 0x8000000000000001L){
    // software interrupt from a machine-mode timer interrupt,
    // forwarded by timervec in kernelvec.S.

    if(cpuid() == 0){
      clockintr();
    }
    
    // acknowledge the software interrupt by clearing
    // the SSIP bit in sip.
    w_sip(r_sip() & ~2);

    return 2;
  } else {
    return 0;
  }
}

