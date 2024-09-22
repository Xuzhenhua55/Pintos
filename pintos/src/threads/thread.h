#ifndef THREADS_THREAD_H
#define THREADS_THREAD_H

#include <debug.h>
#include <list.h>
#include <stdint.h>
#include "fixed-point.h"
#include "threads/synch.h"
/* States in a thread's life cycle. */
enum thread_status
{
  THREAD_RUNNING, /* Running thread. */
  THREAD_READY,   /* Not running but ready to run. */
  THREAD_BLOCKED, /* Waiting for an event to trigger. */
  THREAD_DYING    /* About to be destroyed. */
};

/* Thread identifier type.
   You can redefine this to whatever type you like. */
typedef int tid_t;
#define TID_ERROR ((tid_t)-1) /* Error value for tid_t. */

/* Thread priorities. */
#define PRI_MIN 0              /* Lowest priority. */
#define PRI_DEFAULT 31         /* Default priority. */
#define PRI_MAX 63             /* Highest priority. */
#define PRI_DONATE_MAX_DEPTH 8 // 优先级捐献最大深度
#define FD_MAX 128             // 可创建的文件最大数量

// 子进程的全部信息：不能仅存储子进程本身，而是需要将其中关键的信息也抽取出来
struct child_entry
{
  tid_t tid;                  // 子进程的tid
  struct thread *t;           // 子进程本身的指针，当它不是活动状态时设置为NULL
  bool is_alive;              // 判断子进程是否为活动状态
  int exit_code;              // 子进程的退出状态
  bool is_waiting_on;         // 判断是否一个父进程正在等待该子进程
  struct semaphore wait_sema; // 用于同步父进程对子进程的等待
  struct list_elem elem;
};

// 某个线程所打开的某个文件的管理结构
struct file_entry
{
  int fd;         /**< File descriptor. */
  struct file *f; /**< Pointer to file. */
  struct list_elem elem;
};
/* A kernel thread or user process.

   Each thread structure is stored in its own 4 kB page.  The
   thread structure itself sits at the very bottom of the page
   (at offset 0).  The rest of the page is reserved for the
   thread's kernel stack, which grows downward from the top of
   the page (at offset 4 kB).  Here's an illustration:

        4 kB +---------------------------------+
             |          kernel stack           |
             |                |                |
             |                |                |
             |                V                |
             |         grows downward          |
             |                                 |
             |                                 |
             |                                 |
             |                                 |
             |                                 |
             |                                 |
             |                                 |
             |                                 |
             +---------------------------------+
             |              magic              |
             |                :                |
             |                :                |
             |               name              |
             |              status             |
        0 kB +---------------------------------+

   The upshot of this is twofold:

      1. First, `struct thread' must not be allowed to grow too
         big.  If it does, then there will not be enough room for
         the kernel stack.  Our base `struct thread' is only a
         few bytes in size.  It probably should stay well under 1
         kB.

      2. Second, kernel stacks must not be allowed to grow too
         large.  If a stack overflows, it will corrupt the thread
         state.  Thus, kernel functions should not allocate large
         structures or arrays as non-static local variables.  Use
         dynamic allocation with malloc() or palloc_get_page()
         instead.

   The first symptom of either of these problems will probably be
   an assertion failure in thread_current(), which checks that
   the `magic' member of the running thread's `struct thread' is
   set to THREAD_MAGIC.  Stack overflow will normally change this
   value, triggering the assertion. */
/* The `elem' member has a dual purpose.  It can be an element in
   the run queue (thread.c), or it can be an element in a
   semaphore wait list (synch.c).  It can be used these two ways
   only because they are mutually exclusive: only a thread in the
   ready state is on the run queue, whereas only a thread in the
   blocked state is on a semaphore wait list. */
struct thread
{
  /* Owned by thread.c. */
  tid_t tid;                 /* Thread identifier. */
  enum thread_status status; /* Thread state. */
  int exit_code;
  char name[16];  /* Name (for debugging purposes). */
  uint8_t *stack; /* Saved stack pointer. */
  int priority;   /* Priority. */

  int init_priority;
  struct lock *await_lock;
  struct list locks_possess_list;

  struct list_elem allelem; /* List element for all threads list. */

  /* Shared between thread.c and synch.c. */
  struct list_elem elem;  /* List element. */
  int nice;               // 每个线程都有一个整数nice值该值确定该线程与其他线程应该有多“不错”[-20,20]
  fixed_point recent_cpu; // 线程最近使用的CPU的时间的估计值
#ifdef USERPROG
  /* Owned by userprog/process.c. */
  uint32_t *pagedir; /* Page directory. */
#endif

  /* Owned by thread.c. */
  unsigned magic; /* Detects stack overflow. */

  struct thread *parent;        // 该进程的父进程
  struct list child_list;       // 该进程的子进程列表，每一个元素为child_entry类型的
  struct child_entry *as_child; // 该进程本身作为子进程时维护的结构

  struct semaphore exec_sema; // 用于父进程等待成功加载子进程的可执行文件时的阻塞
  bool exec_success;          // 判断加载是否成功

  struct file *exec_file; // 由线程创建的可执行文件
  struct list file_list;  // 该线程的可执行文件列表，每一个元素为file_entry
  int next_fd;            // 下一个被分配的文件描述符

  struct dir *dir; // 该线程所处的目录位置
};

/* If false (default), use round-robin scheduler.
   If true, use multi-level feedback queue scheduler.
   Controlled by kernel command-line option "-o mlfqs". */
extern bool thread_mlfqs;

void thread_init(void);
void thread_start(void);

void thread_tick(void);
void thread_print_stats(void);

typedef void thread_func(void *aux);
tid_t thread_create(const char *name, int priority, thread_func *, void *);

void thread_block(void);
void thread_unblock(struct thread *);

struct thread *thread_current(void);
tid_t thread_tid(void);
const char *thread_name(void);

void thread_exit(void) NO_RETURN;
void thread_yield(void);

/* Performs some operation on thread t, given auxiliary data AUX. */
typedef void thread_action_func(struct thread *t, void *aux);
void thread_foreach(thread_action_func *, void *);

int thread_get_priority(void);
void thread_set_priority(int);

int thread_get_nice(void);
void thread_set_nice(int);
int thread_get_recent_cpu(void);
int thread_get_load_avg(void);

// 根据tid来终结某个进程
int thread_dead(tid_t tid);
#endif /* threads/thread.h */
