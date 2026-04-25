// Buffer cache.
//
// The buffer cache is a linked list of buf structures holding
// cached copies of disk block contents.  Caching disk blocks
// in memory reduces the number of disk reads and also provides
// a synchronization point for disk blocks used by multiple processes.
//
// Interface:
// * To get a buffer for a particular disk block, call bread.
// * After changing buffer data, call bwrite to write it to disk.
// * When done with the buffer, call brelse.
// * Do not use the buffer after calling brelse.
// * Only one process at a time can use a buffer,
//     so do not keep them longer than necessary.

#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "buf.h"

#define NBUCKET 13 // 哈希表中桶的数量

struct
{
  struct spinlock lock; // 全局锁，在偷空闲 buf 时使用
  struct buf buf[NBUF];

  // Linked list of all buffers, through prev/next.
  // Sorted by how recently the buffer was used.
  // head.next is most recent, head.prev is least.
  // struct buf head;

  struct buf heads[NBUCKET];          // 哈希表里面的桶
  struct spinlock buck_lock[NBUCKET]; // 每个桶配一块锁
} bcache;

void binit(void)
{
  struct buf *b;

  initlock(&bcache.lock, "bcache_glob");
  for (int i = 0; i < NBUCKET; ++i)
  {
    initlock(&bcache.buck_lock[i], "bcache_bucket");
  }

  // Create linked list of buffers
  // bcache.head.prev = &bcache.head;
  // bcache.head.next = &bcache.head;
  // for (b = bcache.buf; b < bcache.buf + NBUF; b++)
  // {
  //   b->next = bcache.head.next;
  //   b->prev = &bcache.head;
  //   initsleeplock(&b->lock, "buffer");
  //   bcache.head.next->prev = b;
  //   bcache.head.next = b;
  // }

  // 初始化时将所有的缓存块先放在第0个桶内
  for (int i = 0; i < NBUCKET; ++i)
  {
    bcache.heads[i].prev = &bcache.heads[i];
    bcache.heads[i].next = &bcache.heads[i];
  }
  for (b = bcache.buf; b < bcache.buf + NBUF; b++)
  {
    b->next = bcache.heads[0].next;
    b->prev = &bcache.heads[0];
    initsleeplock(&b->lock, "buffer");
    bcache.heads[0].next->prev = b;
    bcache.heads[0].next = b;
  }
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf *
bget(uint dev, uint blockno)
{
  struct buf *b;

  // acquire(&bcache.lock);

  int id = blockno % NBUCKET;
  acquire(&bcache.buck_lock[id]);

  // Is the block already cached?
  for (b = bcache.heads[id].next; b != &bcache.heads[id]; b = b->next)
  {
    if (b->dev == dev && b->blockno == blockno)
    {
      b->refcnt++;
      release(&bcache.buck_lock[id]);
      acquiresleep(&b->lock);
      return b;
    }
  }
  release(&bcache.buck_lock[id]);

  // Not cached.
  acquire(&bcache.lock); // 全局驱逐锁，避免多个副本

  // 获取全局锁的期间，blockno可能被别的进程加载进来，重新检查
  acquire(&bcache.buck_lock[id]);
  for (b = bcache.heads[id].next; b != &bcache.heads[id]; b = b->next)
  {
    if (b->dev == dev && b->blockno == blockno)
    {
      b->refcnt++;
      release(&bcache.buck_lock[id]);
      release(&bcache.lock);
      acquiresleep(&b->lock);
      return b;
    }
  }

  // 仍未被加载进来
  // 先在当前桶内寻找空闲块
  for (b = bcache.heads[id].prev; b != &bcache.heads[id]; b = b->prev)
  {
    if (b->refcnt == 0)
    {
      b->dev = dev;
      b->blockno = blockno;
      b->valid = 0;
      b->refcnt = 1;
      release(&bcache.buck_lock[id]);
      release(&bcache.lock);
      acquiresleep(&b->lock);
      return b;
    }
  }
  release(&bcache.buck_lock[id]);

  // 当前桶内没有空闲块，去别的桶内窃取空闲块
  for (int i = 0; i < NBUCKET; ++i)
  {
    if (i != id)
    {
      acquire(&bcache.buck_lock[i]);
      struct buf *p;
      for (p = bcache.heads[i].prev; p != &bcache.heads[i]; p = p->prev)
      {
        if (p->refcnt == 0)
        {
          p->dev = dev;
          p->blockno = blockno;
          p->valid = 0;
          p->refcnt = 1;

          // 从第i个桶中删除这个缓存块
          p->next->prev = p->prev;
          p->prev->next = p->next;
          release(&bcache.buck_lock[i]);
          // 向第id个桶中添加缓存块
          acquire(&bcache.buck_lock[id]);
          p->next = bcache.heads[id].next;
          p->prev = &bcache.heads[id];
          bcache.heads[id].next->prev = p;
          bcache.heads[id].next = p;
          release(&bcache.buck_lock[id]);
          release(&bcache.lock);
          acquiresleep(&p->lock);
          return p;
        }
      }
      release(&bcache.buck_lock[i]);
    }
  }
  release(&bcache.lock);

  panic("bget: no buffers");
}

// Return a locked buf with the contents of the indicated block.
struct buf *
bread(uint dev, uint blockno)
{
  struct buf *b;

  b = bget(dev, blockno);
  if (!b->valid)
  {
    virtio_disk_rw(b, 0);
    b->valid = 1;
  }
  return b;
}

// Write b's contents to disk.  Must be locked.
void bwrite(struct buf *b)
{
  if (!holdingsleep(&b->lock))
    panic("bwrite");
  virtio_disk_rw(b, 1);
}

// Release a locked buffer.
// Move to the head of the most-recently-used list.
void brelse(struct buf *b)
{
  if (!holdingsleep(&b->lock))
    panic("brelse");

  releasesleep(&b->lock);

  int id = b->blockno % NBUCKET;

  acquire(&bcache.buck_lock[id]);
  b->refcnt--;
  // if (b->refcnt == 0)
  // {
  //   // no one is waiting for it.
  //   b->next->prev = b->prev;
  //   b->prev->next = b->next;
  //   b->next = bcache.head.next;
  //   b->prev = &bcache.head;
  //   bcache.head.next->prev = b;
  //   bcache.head.next = b;
  // }

  release(&bcache.buck_lock[id]);
}

void bpin(struct buf *b)
{
  int id = b->blockno % NBUCKET;
  acquire(&bcache.buck_lock[id]);
  b->refcnt++;
  release(&bcache.buck_lock[id]);
}

void bunpin(struct buf *b)
{
  int id = b->blockno % NBUCKET;
  acquire(&bcache.buck_lock[id]);
  b->refcnt--;
  release(&bcache.buck_lock[id]);
}
