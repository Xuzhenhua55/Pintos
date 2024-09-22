#include "devices/timer.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include "devices/pit.h"
#include "threads/interrupt.h"
#include "threads/synch.h"
#include "threads/thread.h"

/* See [8254] for hardware details of the 8254 timer chip. */

#if TIMER_FREQ < 19
#error 8254 timer requires TIMER_FREQ >= 19
#endif
#if TIMER_FREQ > 1000
#error TIMER_FREQ <= 1000 recommended
#endif

struct sleep_thread_entry
{
  struct thread *sleep_thread;
  int64_t sleep_ticks;
  struct list_elem elem;
};

/* Number of timer ticks since OS booted. */
static int64_t ticks;

/* Number of loops per timer tick.
   Initialized by timer_calibrate(). */
static unsigned loops_per_tick;

static struct list sleep_thread_list;

static intr_handler_func timer_interrupt;
static bool too_many_loops(unsigned loops);
static void busy_wait(int64_t loops);
static void real_time_sleep(int64_t num, int32_t denom);
static void real_time_delay(int64_t num, int32_t denom);
static bool thread_priority_greater(const struct list_elem *lhs, const struct list_elem *rhs, void *aux UNUSED);
/* Sets up the timer to interrupt TIMER_FREQ times per second,
   and registers the corresponding interrupt. */
void timer_init(void)
{
  list_init(&sleep_thread_list);
  pit_configure_channel(0, 2, TIMER_FREQ);
  intr_register_ext(0x20, timer_interrupt, "8254 Timer");
}

/* Calibrates loops_per_tick, used to implement brief delays. */
void timer_calibrate(void)
{
  unsigned high_bit, test_bit;

  ASSERT(intr_get_level() == INTR_ON);
  printf("Calibrating timer...  ");

  /* Approximate loops_per_tick as the largest power-of-two
     still less than one timer tick. */
  loops_per_tick = 1u << 10;
  while (!too_many_loops(loops_per_tick << 1))
  {
    loops_per_tick <<= 1;
    ASSERT(loops_per_tick != 0);
  }

  /* Refine the next 8 bits of loops_per_tick. */
  high_bit = loops_per_tick;
  for (test_bit = high_bit >> 1; test_bit != high_bit >> 10; test_bit >>= 1)
    if (!too_many_loops(loops_per_tick | test_bit))
      loops_per_tick |= test_bit;

  printf("%'" PRIu64 " loops/s.\n", (uint64_t)loops_per_tick * TIMER_FREQ);
}

/* Returns the number of timer ticks since the OS booted. */
int64_t
timer_ticks(void)
{
  enum intr_level old_level = intr_disable();
  int64_t t = ticks;
  intr_set_level(old_level);
  return t;
}

/* Returns the number of timer ticks elapsed since THEN, which
   should be a value once returned by timer_ticks(). */
int64_t
timer_elapsed(int64_t then)
{
  return timer_ticks() - then;
}
// 线程优先级的比较函数
static bool
thread_priority_greater(const struct list_elem *lhs, const struct list_elem *rhs, void *aux UNUSED)
{

  return list_entry(lhs, struct sleep_thread_entry, elem)->sleep_thread->priority > list_entry(rhs, struct sleep_thread_entry, elem)->sleep_thread->priority;
}
/* Sleeps for approximately TICKS timer ticks.  Interrupts must
   be turned on. */
void timer_sleep(int64_t ticks)
{
  if (ticks < 0)
    return;

  enum intr_level old_level = intr_disable();

  struct sleep_thread_entry *entry = (struct sleep_thread_entry *)malloc(sizeof(struct sleep_thread_entry));
  entry->sleep_thread = thread_current();
  entry->sleep_ticks = ticks;
  list_insert_ordered(&sleep_thread_list, &entry->elem, (list_less_func *)&thread_priority_greater, NULL); // 按照优先级插入就绪队列
  thread_block();

  intr_set_level(old_level);
}

static void
sleep_thread_list_tick(void)
{
  struct list_elem *_elem = list_begin(&sleep_thread_list);
  while (_elem != list_end(&sleep_thread_list))
  {
    struct sleep_thread_entry *entry = list_entry(_elem, struct sleep_thread_entry, elem);
    if (--(entry->sleep_ticks) <= 0)
    {
      _elem = list_remove(_elem);
      thread_unblock(entry->sleep_thread);
      // 由于timer唤醒导致抢占式调度的情况可能测试用例中没有完全覆盖
      // 此时如果唤醒的线程的优先级比当前线程更高那么在中断结束前进行重调度
      if (entry->sleep_thread->priority > thread_current()->priority)
      {
        intr_yield_on_return();
      }
    }
    else
    {
      _elem = list_next(_elem);
    }
  }
}

/* Sleeps for approximately MS milliseconds.  Interrupts must be
   turned on. */
void timer_msleep(int64_t ms)
{
  real_time_sleep(ms, 1000);
}

/* Sleeps for approximately US microseconds.  Interrupts must be
   turned on. */
void timer_usleep(int64_t us)
{
  real_time_sleep(us, 1000 * 1000);
}

/* Sleeps for approximately NS nanoseconds.  Interrupts must be
   turned on. */
void timer_nsleep(int64_t ns)
{
  real_time_sleep(ns, 1000 * 1000 * 1000);
}

/* Busy-waits for approximately MS milliseconds.  Interrupts need
   not be turned on.

   Busy waiting wastes CPU cycles, and busy waiting with
   interrupts off for the interval between timer ticks or longer
   will cause timer ticks to be lost.  Thus, use timer_msleep()
   instead if interrupts are enabled. */
void timer_mdelay(int64_t ms)
{
  real_time_delay(ms, 1000);
}

/* Sleeps for approximately US microseconds.  Interrupts need not
   be turned on.

   Busy waiting wastes CPU cycles, and busy waiting with
   interrupts off for the interval between timer ticks or longer
   will cause timer ticks to be lost.  Thus, use timer_usleep()
   instead if interrupts are enabled. */
void timer_udelay(int64_t us)
{
  real_time_delay(us, 1000 * 1000);
}

/* Sleeps execution for approximately NS nanoseconds.  Interrupts
   need not be turned on.

   Busy waiting wastes CPU cycles, and busy waiting with
   interrupts off for the interval between timer ticks or longer
   will cause timer ticks to be lost.  Thus, use timer_nsleep()
   instead if interrupts are enabled.*/
void timer_ndelay(int64_t ns)
{
  real_time_delay(ns, 1000 * 1000 * 1000);
}

/* Prints timer statistics. */
void timer_print_stats(void)
{
  printf("Timer: %" PRId64 " ticks\n", timer_ticks());
}

/* Timer interrupt handler. */
static void
timer_interrupt(struct intr_frame *args UNUSED)
{
  ticks++;
  sleep_thread_list_tick();
  thread_tick();
}

/* Returns true if LOOPS iterations waits for more than one timer
   tick, otherwise false. */
static bool
too_many_loops(unsigned loops)
{
  /* Wait for a timer tick. */
  int64_t start = ticks;
  while (ticks == start)
    barrier();

  /* Run LOOPS loops. */
  start = ticks;
  busy_wait(loops);

  /* If the tick count changed, we iterated too long. */
  barrier();
  return start != ticks;
}

/* Iterates through a simple loop LOOPS times, for implementing
   brief delays.

   Marked NO_INLINE because code alignment can significantly
   affect timings, so that if this function was inlined
   differently in different places the results would be difficult
   to predict. */
static void NO_INLINE
busy_wait(int64_t loops)
{
  while (loops-- > 0)
    barrier();
}

/* Sleep for approximately NUM/DENOM seconds. */
static void
real_time_sleep(int64_t num, int32_t denom)
{
  /* Convert NUM/DENOM seconds into timer ticks, rounding down.

        (NUM / DENOM) s
     ---------------------- = NUM * TIMER_FREQ / DENOM ticks.
     1 s / TIMER_FREQ ticks
  */
  int64_t ticks = num * TIMER_FREQ / denom;

  ASSERT(intr_get_level() == INTR_ON);
  if (ticks > 0)
  {
    /* We're waiting for at least one full timer tick.  Use
       timer_sleep() because it will yield the CPU to other
       processes. */
    timer_sleep(ticks);
  }
  else
  {
    /* Otherwise, use a busy-wait loop for more accurate
       sub-tick timing. */
    real_time_delay(num, denom);
  }
}

/* Busy-wait for approximately NUM/DENOM seconds. */
static void
real_time_delay(int64_t num, int32_t denom)
{
  /* Scale the numerator and denominator down by 1000 to avoid
     the possibility of overflow. */
  ASSERT(denom % 1000 == 0);
  busy_wait(loops_per_tick * num / 1000 * TIMER_FREQ / (denom / 1000));
}
