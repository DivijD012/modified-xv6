// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

void freerange(void *pa_start, void *pa_end);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.
#ifdef COW
int page_refs[PAGE_COUNT];
struct spinlock page_refs_lock;
#endif

struct run {
  struct run *next;
};

struct {
  struct spinlock lock;
  struct run *freelist;
} kmem;

void
kinit()
{
#ifdef COW
  initlock(&page_refs_lock, "page refs");
  acquire(&page_refs_lock);
  for(int i = 0; i < PAGE_COUNT; i++){
    page_refs[i] = 1; 
  }
  page_refs[0] = 2;
  release(&page_refs_lock);
#endif
  initlock(&kmem.lock, "kmem");
  freerange(end, (void*)PHYSTOP);
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE)
    kfree(p);
}

// Free the page of physical memory pointed at by pa,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(void *pa)
{
#ifdef COW
  int page_num = ((uint64)pa / PGSIZE);
  if(page_num >= PAGE_COUNT)
    panic("kfree: More than page count");
  
  acquire(&page_refs_lock);
  page_refs[page_num]--; // decrease page refs
  // if(page_num == 0)
        // printf("mum mum2: %d %d\n", page_num, page_refs[page_num]);
  if(page_refs[page_num] <= 0)
    page_refs[page_num] = 0;
  // If there are still references dont free
  if(page_refs[page_num] > 0){
    release(&page_refs_lock);
    return;
  }
  release(&page_refs_lock);
#endif

  struct run *r;
  if(((uint64)pa % PGSIZE) != 0)
    panic("kfree: not page address");
  if((char*)pa < end){
    if(pa == 0)
      // return;
      panic("kfree: cannot free 0x0");
    panic("kfree: trying to free kernel page");
  }
  if((uint64)pa >= PHYSTOP)
    panic("kfree: address more thena PHYSTOP");

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  r = (struct run*)pa;
  
  acquire(&kmem.lock);
  r->next = kmem.freelist;
  kmem.freelist = r;
  release(&kmem.lock);
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;

  acquire(&kmem.lock);
  r = kmem.freelist;
  if(r)
    kmem.freelist = r->next;
  release(&kmem.lock);

  if(r)
    memset((char*)r, 5, PGSIZE); // fill with junk
#ifdef COW
  int page_num = ((uint64)r / PGSIZE);
  acquire(&page_refs_lock);
  page_refs[page_num]++;
  release(&page_refs_lock);
#endif
  return (void*)r;
}
