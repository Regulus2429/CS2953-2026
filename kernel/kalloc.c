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

int ref_count[PHYSTOP / PGSIZE]; // 每个页面的引用计数
struct spinlock ref_lock;        // 维护引用计数的锁

// 增加某个页的引用计数
void incrref(uint64 pa)
{
  uint64 idx = (uint64)pa / PGSIZE;
  acquire(&ref_lock);
  if (ref_count[idx] < 0)
  {
    ref_count[idx] = 0;
  }
  ++ref_count[idx];
  release(&ref_lock);
}

// 减少某个页的引用计数
void decrref(uint64 pa)
{
  uint64 idx = (uint64)pa / PGSIZE;
  acquire(&ref_lock);
  --ref_count[idx];
  release(&ref_lock);
}

// 获取某个页面的引用计数
int get_ref_count(uint64 pa)
{
  uint64 idx = (uint64)pa / PGSIZE;
  int res;
  acquire(&ref_lock);
  res = ref_count[idx];
  release(&ref_lock);
  return res;
}

struct run
{
  struct run *next;
};

struct
{
  struct spinlock lock;
  struct run *freelist;
} kmem[NCPU]; // 修改为数值，每个 CPU 各一个

void kinit()
{
  // 对每个 CPU 的锁初始化
  for (int i = 0; i < NCPU; ++i)
  {
    initlock(&kmem[i].lock, "kmem");
  }

  // 对引用计数的锁初始化
  initlock(&ref_lock, "ref_lock");

  // initlock(&kmem.lock, "kmem");
  freerange(end, (void *)PHYSTOP);
}

void freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char *)PGROUNDUP((uint64)pa_start);
  for (; p + PGSIZE <= (char *)pa_end; p += PGSIZE)
    kfree(p);
}

// Free the page of physical memory pointed at by pa,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void kfree(void *pa)
{
  struct run *r;

  if (((uint64)pa % PGSIZE) != 0 || (char *)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  acquire(&ref_lock);
  uint64 idx = (uint64)pa / PGSIZE;
  --ref_count[idx];

  if (ref_count[idx] <= 0)
  {
    ref_count[idx] = 0;
    release(&ref_lock); // 先释放ref_lock, 避免死锁

    // Fill with junk to catch dangling refs.
    memset(pa, 1, PGSIZE);

    r = (struct run *)pa;

    push_off();
    int id = cpuid();

    acquire(&kmem[id].lock);
    r->next = kmem[id].freelist;
    kmem[id].freelist = r;
    release(&kmem[id].lock);

    pop_off();
  }
  else
  {
    release(&ref_lock);
  }
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;

  push_off();
  int id = cpuid();

  acquire(&kmem[id].lock);
  r = kmem[id].freelist;
  if (r)
    kmem[id].freelist = r->next;
  release(&kmem[id].lock);

  if (r)
  // 成功在自己的链表里拿到一页
  {
    memset((char *)r, 5, PGSIZE); // fill with junk

    acquire(&ref_lock);
    ref_count[(uint64)r / PGSIZE] = 1;
    release(&ref_lock);
  }
  else
  {
    // 尝试别的 cpu 的freelist
    for (int i = 0; i < NCPU; ++i)
    {
      if (i == id)
        continue;

      acquire(&kmem[i].lock);
      if (kmem[i].freelist)
      {
        // 每次只取一页
        // r = kmem[i].freelist;
        // kmem[i].freelist = r->next;
        // memset((char *)r, 5, PGSIZE); // fill with junk

        // 每次取多页
        r = kmem[i].freelist;
        struct run *tail = r;
        int steal_count = 1;
        while (steal_count < 64 && tail->next)
        {
          tail = tail->next;
          ++steal_count;
        }

        kmem[i].freelist = tail->next;
        tail->next = 0;

        release(&kmem[i].lock);

        // 此时 r 到 tail 这串页已经被垄断了，外界碰不到
        // 在把它们放进自己的 freelist 之前，立刻把这一串页的引用计数全部初始化为 1！
        acquire(&ref_lock);
        struct run *curr = r;
        while (curr)
        {
          ref_count[(uint64)curr / PGSIZE] = 1;
          curr = curr->next;
        }
        release(&ref_lock);

        struct run *next_pages = r->next;
        memset((char *)r, 5, PGSIZE); // fill with junk

        if (next_pages)
        {
          acquire(&kmem[id].lock);

          tail->next = kmem[id].freelist; // 偷取的链表接在可能已被别的core塞入空闲页的freelist
          kmem[id].freelist = next_pages;

          release(&kmem[id].lock);
        }
        break;
      }

      // 该 CPU 为空, 没有偷到
      release(&kmem[i].lock);
    }
  }

  pop_off();

  return (void *)r;
}

// collect the amount of free memory
uint64
kfreemem(void)
{
  // 实现思路参考上面的 kalloc
  struct run *r;
  uint64 freemem = 0;

  // 修改kmem之后，相应修改遍历的方式
  for (int i = 0; i < NCPU; ++i)
  {
    acquire(&kmem[i].lock);
    r = kmem[i].freelist;
    while (r)
    {
      freemem += PGSIZE;
      r = r->next;
    }
    release(&kmem[i].lock);
  }

  return freemem;
}