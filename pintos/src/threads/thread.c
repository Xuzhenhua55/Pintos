#include "threads/thread.h"
#include <debug.h>
#include <stddef.h>
#include <random.h>
#include <stdio.h>
#include <string.h>
#include "threads/flags.h"
#include "threads/interrupt.h"
#include "threads/intr-stubs.h"
#include "threads/palloc.h"
#include "threads/switch.h"
#include "threads/synch.h"
#include "threads/vaddr.h"
#include "devices/timer.h"
#include "filesys/filesys.h"
#include "filesys/file.h"
#include "filesys/directory.h"
#ifdef USERPROG
#include "userprog/process.h"
#endif

static bool ready_to_schedule;
static fixed_point load_avg;

/* Random value for struct thread's `magic' member.
   Used to detect stack overflow.  See the big comment at the top
   of thread.h for details. */
#define THREAD_MAGIC 0xcd6abf4b

/* List of processes in THREAD_READY state, that is, processes
   that are ready to run but not actually running. */
static struct list ready_list;

/* List of all processes.  Processes are added to this list
   when they are first scheduled and removed when they exit. */
static struct list all_list;

/* Idle thread. */
static struct thread *idle_thread;

/* Initial thread, the thread running init.c:main(). */
static struct thread *initial_thread;

/* Lock used by allocate_tid(). */
static struct lock tid_lock;

/* Stack frame for kernel_thread(). */
struct kernel_thread_frame
{
  void *eip;             /* Return address. */
  thread_func *function; /* Function to call. */
  void *aux;             /* Auxiliary data for function. */
};

/* Statistics. */
static long long idle_ticks;   /* # of timer ticks spent idle. */
static long long kernel_ticks; /* # of timer ticks in kernel threads. */
static long long user_ticks;   /* # of timer ticks in user programs. */

/* Scheduling. */
#define TIME_SLICE 4          /* # of timer ticks to give each thread. */
static unsigned thread_ticks; /* # of timer ticks since last yield. */

/* If false (default), use round-robin scheduler.
   If true, use multi-level feedback queue scheduler.
   Controlled by kernel command-line option "-o mlfqs". */
bool thread_mlfqs;

static void kernel_thread(thread_func *, void *aux);

static void idle(void *aux UNUSED);
static struct thread *running_thread(void);
static struct thread *next_thread_to_run(void);
static void init_thread(struct thread *, const char *name, int priority);
static bool is_thread(struct thread *) UNUSED;
static void *alloc_frame(struct thread *, size_t size);
static void schedule(void);
void thread_schedule_tail(struct thread *prev);
static tid_t allocate_tid(void);

// 根据tid来终结某个进程
int thread_dead(tid_t tid)
{
  struct list_elem *elem_;
  for (elem_ = list_begin(&all_list); elem_ != list_end(&all_list); elem_ = list_next(elem_))
  {
    struct thread *thread_ = list_entry(elem_, struct thread, allelem);
    if (thread_->tid = tid)
      return 0;
  }
  return 1;
}

static bool
thread_priority_greater(const struct list_elem *lhs, const struct list_elem *rhs, void *aux UNUSED)
{
  return list_entry(lhs, struct thread, elem)->priority > list_entry(rhs, struct thread, elem)->priority;
}
// 针对每秒更新一次load_avg的需求编写对应的函数
static void
update_load_avg_mlfqs(void)
{
  // 计算表达式中的系数并获得就绪队列中的进程数量同时判断此时正在运行的线程是否是idle_list
  fixed_point coefficient = divide_ff(itof(59), itof(60));
  int ready_threads = list_size(&ready_list);
  // 按照pintos文档中的需求，需要将正在运行的线程也算在ready_threads中
  if (thread_current() != idle_thread)
  {
    ready_threads++;
  }
  load_avg = multiply_ff(coefficient, load_avg) + multiply_ff(divide_ff(itof(1), itof(60)), itof(ready_threads));
}
// 针对每秒更新一次线程最近使用CPU时间的需求编写对应的函数
static void update_recent_cpu_mlfqs(struct thread *t, void *aux UNUSED)
{
  if (t != idle_thread)
  {
    // 在文档中提示我们建议先计算recent_cpu的系数然后再相乘否则load_avg和recent_cpu直接相乘会导致溢出
    fixed_point coefficient = divide_ff(2 * load_avg, 2 * load_avg + itof(1));
    t->recent_cpu = multiply_ff(coefficient, t->recent_cpu) + itof(t->nice);
  }
}
// 针对每四秒更新一次线程的优先级编写对于的函数
static void update_priority_mlfqs(struct thread *t, void *aux UNUSED)
{
  if (t != idle_thread)
  {
    t->priority = PRI_MAX - ftoi(t->recent_cpu / 4) - 2 * t->nice;
    // 需要注意的是计算完之后需要和PRI_MAX以及PRI_MIN进行比较
    t->priority = t->priority > PRI_MAX ? PRI_MAX : t->priority < PRI_MIN ? PRI_MIN
                                                                          : t->priority;
  }
}
/* Initializes the threading system by transforming the code
   that's currently running into a thread.  This can't work in
   general and it is possible in this case only because loader.S
   was careful to put the bottom of the stack at a page boundary.

   Also initializes the run queue and the tid lock.

   After calling this function, be sure to initialize the page
   allocator before trying to create any threads with
   thread_create().

   It is not safe to call thread_current() until this function
   finishes. */
void thread_init(void)
{
  ASSERT(intr_get_level() == INTR_OFF);

  lock_init(&tid_lock);
  list_init(&ready_list);
  list_init(&all_list);

  /* Set up a thread structure for the running thread. */
  initial_thread = running_thread();
  init_thread(initial_thread, "main", PRI_DEFAULT);
  initial_thread->status = THREAD_RUNNING;
  initial_thread->tid = allocate_tid();
}

/* Starts preemptive thread scheduling by enabling interrupts.
   Also creates the idle thread. */
void thread_start(void)
{
  ready_to_schedule = true;
  load_avg = itof(0);
  /* Create the idle thread. */
  struct semaphore idle_started;
  sema_init(&idle_started, 0);
  thread_create("idle", PRI_MIN, idle, &idle_started);

  /* Start preemptive thread scheduling. */
  intr_enable();

  /* Wait for the idle thread to initialize idle_thread. */
  sema_down(&idle_started);
}

/* Called by the timer interrupt handler at each timer tick.
   Thus, this function runs in an external interrupt context. */

void thread_tick(void)
{
  struct thread *t = thread_current();

  /* Update statistics. */
  if (t == idle_thread)
    idle_ticks++;
#ifdef USERPROG
  else if (t->pagedir != NULL)
    user_ticks++;
#endif
  else
    kernel_ticks++;

  // 为了适应高级调度的需求需要对thread_mlfq进行判断
  if (thread_mlfqs)
  {
    // 如果当前值为true那么就获取当前的ticks并将当前运行的非闲置进程的最近CPU使用时间增加
    int64_t ticks = timer_ticks();
    if (t != idle_thread)
    {
      t->recent_cpu += itof(1);
    }
    // 每四秒更新一次线程的优先级
    if (ticks % 4 == 0)
    {
      thread_foreach(update_priority_mlfqs, NULL);
    }
    // 每秒更新一次load_avg（系统平均负载）并根据load_avg来计算当前的recent_cpu时间
    if (ticks % TIMER_FREQ == 0)
    {
      update_load_avg_mlfqs();
      thread_foreach(update_recent_cpu_mlfqs, NULL);
    }
  }

  /* Enforce preemption. */
  if (++thread_ticks >= TIME_SLICE)
    intr_yield_on_return();
}

/* Prints thread statistics. */
void thread_print_stats(void)
{
  printf("Thread: %lld idle ticks, %lld kernel ticks, %lld user ticks\n",
         idle_ticks, kernel_ticks, user_ticks);
}

/* Creates a new kernel thread named NAME with the given initial
   PRIORITY, which executes FUNCTION passing AUX as the argument,
   and adds it to the ready queue.  Returns the thread identifier
   for the new thread, or TID_ERROR if creation fails.

   If thread_start() has been called, then the new thread may be
   scheduled before thread_create() returns.  It could even exit
   before thread_create() returns.  Contrariwise, the original
   thread may run for any amount of time before the new thread is
   scheduled.  Use a semaphore or some other form of
   synchronization if you need to ensure ordering.

   The code provided sets the new thread's `priority' member to
   PRIORITY, but no actual priority scheduling is implemented.
   Priority scheduling is the goal of Problem 1-3. */
tid_t thread_create(const char *name, int priority,
                    thread_func *function, void *aux)
{
  struct thread *t;
  struct kernel_thread_frame *kf;
  struct switch_entry_frame *ef;
  struct switch_threads_frame *sf;
  tid_t tid;

  ASSERT(function != NULL);

  /* Allocate thread. */
  t = palloc_get_page(PAL_ZERO);
  if (t == NULL)
    return TID_ERROR;

  /* Initialize thread. */
  init_thread(t, name, priority);
  tid = t->tid = allocate_tid();
  // 为父子进程相关的结构和其参数初始化
  t->as_child = malloc(sizeof(struct child_entry));
  t->as_child->tid = tid;
  t->as_child->t = t;
  t->as_child->is_alive = true;
  t->as_child->exit_code = 0;
  t->as_child->is_waiting_on = false;
  sema_init(&t->as_child->wait_sema, 0);
  // 将该进程与创建该进程的父进程相链接
  t->parent = thread_current();
  list_push_back(&t->parent->child_list, &t->as_child->elem);
  /* Stack frame for kernel_thread(). */
  kf = alloc_frame(t, sizeof *kf);
  kf->eip = NULL;
  kf->function = function;
  kf->aux = aux;

  /* Stack frame for switch_entry(). */
  ef = alloc_frame(t, sizeof *ef);
  ef->eip = (void (*)(void))kernel_thread;

  /* Stack frame for switch_threads(). */
  sf = alloc_frame(t, sizeof *sf);
  sf->eip = switch_entry;
  sf->ebp = 0;
  // 为当前线程设置目录
  if (thread_current()->dir)
    t->dir = dir_reopen(thread_current()->dir);
  else
    t->dir = NULL;

  /* Add to run queue. */
  thread_unblock(t);
  // 对于开中断来说只需要在新建线程的优先级大于当前线程时进行让步重新调度即可
  if (priority > thread_current()->priority)
  {
    thread_yield();
  }

  return tid;
}

/* Puts the current thread to sleep.  It will not be scheduled
   again until awoken by thread_unblock().

   This function must be called with interrupts turned off.  It
   is usually a better idea to use one of the synchronization
   primitives in synch.h. */
void thread_block(void)
{
  ASSERT(!intr_context());
  ASSERT(intr_get_level() == INTR_OFF);

  thread_current()->status = THREAD_BLOCKED;
  schedule();
}

/* Transitions a blocked thread T to the ready-to-run state.
   This is an error if T is not blocked.  (Use thread_yield() to
   make the running thread ready.)

   This function does not preempt the running thread.  This can
   be important: if the caller had disabled interrupts itself,
   it may expect that it can atomically unblock a thread and
   update other data. */
void thread_unblock(struct thread *t)
{
  enum intr_level old_level;

  ASSERT(is_thread(t));

  old_level = intr_disable();
  ASSERT(t->status == THREAD_BLOCKED);
  // 将线程通过优先级次序插入到就绪队列中
  list_insert_ordered(&ready_list, &t->elem, (list_less_func *)&thread_priority_greater, NULL);
  t->status = THREAD_READY;
  intr_set_level(old_level);
}

/* Returns the name of the running thread. */
const char *
thread_name(void)
{
  return thread_current()->name;
}

/* Returns the running thread.
   This is running_thread() plus a couple of sanity checks.
   See the big comment at the top of thread.h for details. */
struct thread *
thread_current(void)
{
  struct thread *t = running_thread();

  /* Make sure T is really a thread.
     If either of these assertions fire, then your thread may
     have overflowed its stack.  Each thread has less than 4 kB
     of stack, so a few big automatic arrays or moderate
     recursion can cause stack overflow. */
  ASSERT(is_thread(t));
  ASSERT(t->status == THREAD_RUNNING);

  return t;
}

/* Returns the running thread's tid. */
tid_t thread_tid(void)
{
  return thread_current()->tid;
}

/* Deschedules the current thread and destroys it.  Never
   returns to the caller. */
void thread_exit(void)
{
  ASSERT(!intr_context());

#ifdef USERPROG
  process_exit();
#endif
  struct thread *current_thread = thread_current();
  struct list_elem *elem_;
  // 关闭该线程所打开的所有文件
  while (!list_empty(&current_thread->file_list))
  {
    elem_ = list_pop_front(&current_thread->file_list);
    struct file_entry *entry = list_entry(elem_, struct file_entry, elem);
    lock_acquire(&filesys_lock);
    // 首先判断file是否为null，如果不为null
    if (entry->f != NULL)
    {
      // 根据file获取该文件的inode
      struct inode *inode = file_get_inode(entry->f);
      // 如果该inode为null那么继续下一轮
      if (inode == NULL)
        continue;
      // 如果inode是目录那么通过目录关闭来关闭该file
      if (inode_is_dir(inode))
        dir_close(entry->f);
      else // 如果该inode是文件那么通过文件关闭来关闭该file
        file_close(entry->f);
    }

    lock_release(&filesys_lock);
    free(entry);
  }
  // 关闭当前线程的工作目录
  if (thread_current()->dir)
    dir_close(thread_current()->dir);
  // 作为一个父进程
  // 遍历当前进程（即企图结束的进程）的子进程表并通知所有存活的子进程自己已经结束
  for (elem_ = list_begin(&current_thread->child_list); elem_ != list_end(&current_thread->child_list); elem_ = list_next(elem_))
  {
    struct child_entry *entry = list_entry(elem_, struct child_entry, elem);
    if (entry->is_alive)
    {
      entry->t->parent = NULL;
    }
  }
  // 作为一个子进程
  // 如果其父进程已经终结那么其维护as_child已经没有意义，于是可以释放
  if (current_thread->parent == NULL)
  {
    free(current_thread->as_child);
  }
  else
  { // 如果还未终结，那么由于它自身已经终止那么需要把退出码保存到as_child结构中
    current_thread->as_child->exit_code = current_thread->exit_code;
    if (current_thread->as_child->is_waiting_on)
    { // 同时如果存在父进程在等待自己，那么就将父进程从阻塞队列中调出
      sema_up(&current_thread->as_child->wait_sema);
    }
    // 更新其作为子进程结构的信息
    current_thread->as_child->is_alive = false;
    current_thread->as_child->t = NULL;
  }
  /* Remove thread from all threads list, set our status to dying,
     and schedule another process.  That process will destroy us
     when it calls thread_schedule_tail(). */
  intr_disable();
  list_remove(&current_thread->allelem);
  current_thread->status = THREAD_DYING;
  schedule();
  NOT_REACHED();
}

/* Yields the CPU.  The current thread is not put to sleep and
   may be scheduled again immediately at the scheduler's whim. */
void thread_yield(void)
{
  if (!ready_to_schedule)
  {
    return;
  }
  struct thread *cur = thread_current();
  enum intr_level old_level;

  ASSERT(!intr_context());

  old_level = intr_disable();
  if (cur != idle_thread)
    list_insert_ordered(&ready_list, &cur->elem, (list_less_func *)&thread_priority_greater, NULL);
  cur->status = THREAD_READY;
  schedule();
  intr_set_level(old_level);
}

/* Invoke function 'func' on all threads, passing along 'aux'.
   This function must be called with interrupts off. */
void thread_foreach(thread_action_func *func, void *aux)
{
  struct list_elem *e;

  ASSERT(intr_get_level() == INTR_OFF);

  for (e = list_begin(&all_list); e != list_end(&all_list);
       e = list_next(e))
  {
    struct thread *t = list_entry(e, struct thread, allelem);
    func(t, aux);
  }
}

/* Sets the current thread's priority to NEW_PRIORITY. */
void thread_set_priority(int new_priority)
{
  if (!thread_mlfqs)
  {
    struct thread *current_thread = thread_current();
    int old_priority = current_thread->priority;
    // 无论什么情况下当前线程的原始优先级都被改变
    current_thread->init_priority = new_priority;
    if (list_empty(&current_thread->locks_possess_list) || new_priority > current_thread->priority)
    {
      // 只有在当前线程的锁列表为空或者新优先级大于当前优先级时才改变
      current_thread->priority = new_priority;
    }
    // 只有当优先级降低时才可能发生调度
    if (new_priority < old_priority)
    {
      thread_yield();
    }
  }
}

/* Returns the current thread's priority. */
int thread_get_priority(void)
{
  return thread_current()->priority;
}

/* Sets the current thread's nice value to NICE. */
void thread_set_nice(int nice UNUSED)
{
  ASSERT(nice >= -20 && nice <= 20);
  thread_current()->nice = nice;
  update_priority_mlfqs(thread_current(), NULL);
  thread_yield();
}

/* Returns the current thread's nice value. */
int thread_get_nice(void)
{
  return thread_current()->nice;
}

/* Returns 100 times the system load average. */
int thread_get_load_avg(void)
{
  return ftoi_round(100 * load_avg);
}

/* Returns 100 times the current thread's recent_cpu value. */
int thread_get_recent_cpu(void)
{
  return ftoi_round(100 * thread_current()->recent_cpu);
}

/* Idle thread.  Executes when no other thread is ready to run.

   The idle thread is initially put on the ready list by
   thread_start().  It will be scheduled once initially, at which
   point it initializes idle_thread, "up"s the semaphore passed
   to it to enable thread_start() to continue, and immediately
   blocks.  After that, the idle thread never appears in the
   ready list.  It is returned by next_thread_to_run() as a
   special case when the ready list is empty. */
static void
idle(void *idle_started_ UNUSED)
{
  struct semaphore *idle_started = idle_started_;
  idle_thread = thread_current();
  sema_up(idle_started);

  for (;;)
  {
    /* Let someone else run. */
    intr_disable();
    thread_block();

    /* Re-enable interrupts and wait for the next one.

       The `sti' instruction disables interrupts until the
       completion of the next instruction, so these two
       instructions are executed atomically.  This atomicity is
       important; otherwise, an interrupt could be handled
       between re-enabling interrupts and waiting for the next
       one to occur, wasting as much as one clock tick worth of
       time.

       See [IA32-v2a] "HLT", [IA32-v2b] "STI", and [IA32-v3a]
       7.11.1 "HLT Instruction". */
    asm volatile("sti; hlt"
                 :
                 :
                 : "memory");
  }
}

/* Function used as the basis for a kernel thread. */
static void
kernel_thread(thread_func *function, void *aux)
{
  ASSERT(function != NULL);

  intr_enable(); /* The scheduler runs with interrupts off. */
  function(aux); /* Execute the thread function. */
  thread_exit(); /* If function() returns, kill the thread. */
}

/* Returns the running thread. */
struct thread *
running_thread(void)
{
  uint32_t *esp;

  /* Copy the CPU's stack pointer into `esp', and then round that
     down to the start of a page.  Because `struct thread' is
     always at the beginning of a page and the stack pointer is
     somewhere in the middle, this locates the curent thread. */
  asm("mov %%esp, %0"
      : "=g"(esp));
  return pg_round_down(esp);
}

/* Returns true if T appears to point to a valid thread. */
static bool
is_thread(struct thread *t)
{
  return t != NULL && t->magic == THREAD_MAGIC;
}

/* Does basic initialization of T as a blocked thread named
   NAME. */
static void
init_thread(struct thread *t, const char *name, int priority)
{
  enum intr_level old_level;

  ASSERT(t != NULL);
  ASSERT(PRI_MIN <= priority && priority <= PRI_MAX);
  ASSERT(name != NULL);

  memset(t, 0, sizeof *t);
  t->status = THREAD_BLOCKED;

  // 为线程初始化退出状态
  t->exit_code = 0;

  strlcpy(t->name, name, sizeof t->name);
  t->stack = (uint8_t *)t + PGSIZE;
  if (!thread_mlfqs)
  {
    t->priority = priority;
    // 为优先级捐赠相关数据结构进行初始化
    t->init_priority = priority;
  }
  list_init(&t->locks_possess_list);
  t->await_lock = NULL;
  // 为高级调度程序初始化相关数据
  t->recent_cpu = 0;
  t->nice = 0;

  t->magic = THREAD_MAGIC;

  old_level = intr_disable();
  list_push_back(&all_list, &t->allelem);
  intr_set_level(old_level);

  // 为父子进程初始化子进程列表
  list_init(&t->child_list);
  // 初始化exec的等待信号量和
  sema_init(&t->exec_sema, 0);
  t->exec_success = false;

  // 初始化文件管理列表
  list_init(&t->file_list);
  t->next_fd = 2;
  // 初始化线程的当前目录参数
  t->dir = NULL;
}

/* Allocates a SIZE-byte frame at the top of thread T's stack and
   returns a pointer to the frame's base. */
static void *
alloc_frame(struct thread *t, size_t size)
{
  /* Stack data is always allocated in word-size units. */
  ASSERT(is_thread(t));
  ASSERT(size % sizeof(uint32_t) == 0);

  t->stack -= size;
  return t->stack;
}

/* Chooses and returns the next thread to be scheduled.  Should
   return a thread from the run queue, unless the run queue is
   empty.  (If the running thread can continue running, then it
   will be in the run queue.)  If the run queue is empty, return
   idle_thread. */
static struct thread *
next_thread_to_run(void)
{
  if (list_empty(&ready_list))
    return idle_thread;
  else
    return list_entry(list_pop_front(&ready_list), struct thread, elem);
}

/* Completes a thread switch by activating the new thread's page
   tables, and, if the previous thread is dying, destroying it.

   At this function's invocation, we just switched from thread
   PREV, the new thread is already running, and interrupts are
   still disabled.  This function is normally invoked by
   thread_schedule() as its final action before returning, but
   the first time a thread is scheduled it is called by
   switch_entry() (see switch.S).

   It's not safe to call printf() until the thread switch is
   complete.  In practice that means that printf()s should be
   added at the end of the function.

   After this function and its caller returns, the thread switch
   is complete. */
void thread_schedule_tail(struct thread *prev)
{
  struct thread *cur = running_thread();

  ASSERT(intr_get_level() == INTR_OFF);

  /* Mark us as running. */
  cur->status = THREAD_RUNNING;

  /* Start new time slice. */
  thread_ticks = 0;

#ifdef USERPROG
  /* Activate the new address space. */
  process_activate();
#endif

  /* If the thread we switched from is dying, destroy its struct
     thread.  This must happen late so that thread_exit() doesn't
     pull out the rug under itself.  (We don't free
     initial_thread because its memory was not obtained via
     palloc().) */
  if (prev != NULL && prev->status == THREAD_DYING && prev != initial_thread)
  {
    ASSERT(prev != cur);
    palloc_free_page(prev);
  }
}

/* Schedules a new process.  At entry, interrupts must be off and
   the running process's state must have been changed from
   running to some other state.  This function finds another
   thread to run and switches to it.

   It's not safe to call printf() until thread_schedule_tail()
   has completed. */
static void
schedule(void)
{
  struct thread *cur = running_thread();
  struct thread *next = next_thread_to_run();
  struct thread *prev = NULL;

  ASSERT(intr_get_level() == INTR_OFF);
  ASSERT(cur->status != THREAD_RUNNING);
  ASSERT(is_thread(next));

  if (cur != next)
    prev = switch_threads(cur, next);
  thread_schedule_tail(prev);
}

/* Returns a tid to use for a new thread. */
static tid_t
allocate_tid(void)
{
  static tid_t next_tid = 1;
  tid_t tid;

  lock_acquire(&tid_lock);
  tid = next_tid++;
  lock_release(&tid_lock);

  return tid;
}

/* Offset of `stack' member within `struct thread'.
   Used by switch.S, which can't figure it out on its own. */
uint32_t thread_stack_ofs = offsetof(struct thread, stack);
