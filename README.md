# Project 1：Threads

## Mission 1 Alarm Clock

### Data Structures

新建或修改的 struct 或 struct 成员、全局或静态变量、typedef 或枚举的声明

```c
//记录睡眠线程相关数据，作为访问入口
struct sleep_thread_entry
{
  struct thread *sleep_thread;//睡眠线程
  int64_t sleep_ticks;//睡眠线程的睡眠时间
  struct list_elem elem;//列表元素，用于将线程放入双向链接列表中
};

static struct list sleep_thread_list;//睡眠线程阻塞列表
```

新建或修改的函数

```c
//添加初始化睡眠线程阻塞列表部分
void timer_init (void) ;
//避免忙等待的具体实现逻辑
void timer_sleep (int64_t ticks);
//遍历更新睡眠线程阻塞列表中的元素，唤醒到时间的线程
static void sleep_thread_list_tick(void);
//添加执行遍历更新部分
static void timer_interrupt (struct intr_frame *args UNUSED);
```

### Algorithms

&emsp;首先分析`time_sleep`原函数，其实现逻辑为获取机器开机以来经历的 ticks 数并记录为 start 作为忙等待的起始时间，此后确保不存在外部中断的前提下调用`time_elapsed`即获取当前时间和 start 的时间差并与`期望等待时间ticks`进行比较，如果尚未达到，那么就调用`thread_yield`，分析可以发现该函数的作用是让出 CPU 的控制权并将当前线程放入就绪队列中，并重新进行调度，而循环则可以使得该过程不断进行，造成的结果是 CPU 不断进行调度即 BUSY WAIT

&emsp;因此对此改进的思路可以聚焦在循环调度上，可以想到通过阻塞当前线程的方式来取代原先的做法，即使得线程阻塞`期望等待时间ticks`后等待一个管理者被动唤醒。从`timer_init`中可以发现时间中断所注册的中断处理函数是`timer_interrupt`，CPU 在每个时间片都会调用该函数通知时间的更新。于是，我们可以在`timer_sleep`中将线程和睡眠时间记录在一个结构体中，然后将该线程转换为阻塞状态并加入睡眠线程队列，在高频调用的`timer_interrupt`函数中不仅完成`ticks++`同时完成对所有睡眠线程睡眠时间的更新，若降至 0 则将其唤醒（移出睡眠队列）

### Synchronization

&emsp;该设计思路能够适应多线程的需求，若 CPU 对多个线程进行异步处理，那么由于'`timer_sleep`中在阻塞当前队列之前调用了`intr_disable`禁用外部中断，在阻塞完成后恢复先前的阻塞状态，因此多个线程可以同步。同时在唤醒线程的时候可以遍历整个睡眠线程队列。

### Rationale

&emsp;首先在设计上，我不希望为了睡眠这样的非核心功能而修改`thread.h`中关于`thread`结构体的描述信息，其中一种做法是在`thread`结构体中添加`sleep_ticks`，然后进行上述思路的类型处理完成需要，然而该做法将为每一个线程创建都增加空间消耗，尤其是对于许多线程而言并不会用到睡眠功能，因此在本身系统较为复杂、线程数量较多的情况下在`timer.c`文件中进行处理的设计较优。

## Mission 2 Priority Scheduling

### Data Structures

新建或修改的 struct 或 struct 成员、全局或静态变量、typedef 或枚举的声明

```c
struct thread
  {
    ...
    int init_priority;//在保证priority语义不变的前提下代表线程自身的优先级属性
    lock* await_lock;//标识当前线程正在等待的锁，用于获取持有锁的线程的信息
    list locks_possess_list;//标识当前线程所持有的的锁组成的队列，按照优先级排序
  };

struct lock
  {
    ...
    int max_priority_in_query_threads;//标识申请锁的线程的最高优先级
    struct list_elem elem; //用于将锁链接到列表中
  };


  #define PRI_DONATE_MAX_DEPTH 8//最大捐献递归深度
```

新建或修改的函数

```c
//在线程初始化的时候初始化原始优先级、锁列表、所等待的锁
//注意：这一步非常关键，没有处理的话会导致后续很多问题的产生
static void init_thread (struct thread *t, const char *name, int priority)
//将线程加入到就绪队列时按照优先级顺序加入
void thread_unblock (struct thread *t)
//将线程加入到就绪队列时按照优先级顺序加入
void thread_yield (void)
//将list_push_back调用换成list_insert_ordered即通过修改入端来完成需求
void timer_sleep (int64_t ticks);
//list_less_func，用于将优先级更大的线程放到就绪队列的开端
static bool thread_priority_greater(const struct list_elem *lhs,const
struct list_elem* rhs, void *aux UNUSED);
//在线程优先级变化的时候根据新旧优先级的大小关系立即尝试进行调度（Alarm Clock）
//在考虑捐献的情况下，无论何时线程的init_priority需要被更新
//但是线程的priority仅在当前线程的锁列表为空或者优先级小于new priority才改变
void thread_set_priority (int new_priority);
//在开中断中调用thread_unblock后直接直接调用thread_yield
tid_t thread_create (const char *name, int priority, thread_func *function, void *aux)
//在关中断中调用thread_unblock后使用intr_yield_on_return
static void sleep_thread_list_tick(void);
//初始化锁中请求锁的线程中最大优先级为PRI_MIN
void lock_init (struct lock *lock)
//当线程申请锁时判断该锁是否被占有
//如果锁被占有，那么将该锁添加到其await_lock中并更新锁的'所申请线程的最大优先级'
//如果锁的持有者优先级低于当前锁，那么进行'捐献'即将高优先级赋予锁的持有者
//增加多级捐献
void lock_acquire (struct lock *lock)
//list_less_func，用于将优先级更大的锁放到线程所持有锁的队列中
static bool lock_priority_greater(const struct list_elem *lhs,const struct list_elem* rhs, void *aux UNUSED);
//在释放锁时，首先将锁从持有者（当前线程）的锁列表中移除，并将当前线程的优先级恢复
//如果当前线程还持有其他锁且其中最高优先级的锁的优先级高于其原始优先级那么转换
void lock_release (struct lock *lock);
//虽然捐献只考虑锁，但是优先级调度本身需要考虑semaphore和condvar
//因此在解除等待队列中的线程的阻塞状态时，需要将优先级最大的取出并取消阻塞
void sema_up (struct semaphore *sema)
//根据cond的waiters中的元素来获取到到信号量指针来进行比较信号量等待队列中优先级的大小
static bool sema_priority_greater(const struct list_elem *lhs,const struct list_elem* rhs, void *aux UNUSED)
//将原先的list_pop_front式取waiter改为通过比较函数来取优先级最大的waiter
void cond_signal (struct condition *cond, struct lock *lock UNUSED) ;
```

### Algorithms

&emsp;在实现基本的`Alarm Clock`后发现`alarm-priority`测试并未通过，分析`alarm-priority.c`代码后可以发现该测试与线程的优先级调度有关，因此合并到该专题来实现，原先线程调用睡眠函数时是通过`list_push_back`来加入队列的，因此优先级不发挥作用，对`list.c`文件进行剖析后发现可以利用`list_insert_ordered`来对插入函数进行优化，因此`sleep_thread`函数中采用有序插入函数来代替无序插入函数，使用者只需提供一个比较函数即可，这样的结果为在`sleep_thread_list_tick`函数中唤醒线程时，也会按照优先级的次序来依次唤醒，同时修改`thread.c`中的`thread_unblock`，使得线程加入就绪队列也按照顺序来加入即能够完成测试用例的要求。

&emsp;回到本专题的要求，我们可以发现无论有线程是因为状态变化还是优先级变化都要立即尝试进行抢占式调度。因此首先想到修改`thread_set_priority`，即在修改当前线程的优先级时，如果新的优先级低于旧的优先级那么就可能发生调度。此外，线程的状态变化除了主动设置外，还有**线程的创建**和**线程睡眠的唤醒**。因此首先我们想到的是由于这两者都通过`thread_unblock`来将线程插入到就绪队列中，但是我们在分析该函数的注释中发现作者并不希望我们通过该函数来抢占当前线程，因为分析可以知道这两者状态变化分别属于开中断和关中断两种情况（本质区别是是否处于中断上下文中）。对于线程的创建而言，由于并不处于中断上下文中，因此在对新线程和当前线程的优先级进行比较之后可以直接调用`thread_yield`；对于线程睡眠的唤醒而言，由于处在中断上下文中，因此如果唤醒的线程优先级高于当前线程，那么也只能通过`intr_yield_on_return`来使得中断处理程序在中断处理结束返回前进行调度，对于这种情况分析测试用例后发现并未包含睡眠线程唤醒的测试。

&emsp;到此为止的优先级调度仍然存在一个问题，即任务说明中的“捐献”问题。因此我们总结后可以归纳两个基本要求：1.一个线程可能持有多个锁，使得多个线程同时向一个线程捐献。2.多层级捐献模式。本部分的修改相对而言更加复杂，因此主要分为三个思考阶段来完成

1. 首先，当线程因 Donation 而发生优先级变化时，原结构体中的`priority`属性将会发生更改，即可能因为受到捐献而提升，向外表现出该线程此时的优先级，然而未来将锁释放时，不得不**考虑**恢复原有的优先级，因此需要引入`init_priority`来恢复线程的原始优先级。
2. 其次，考虑到上述的两个基本要求，对于第一个要求而言，针对该锁只需关注其中捐献最大的优先级，所以需要为该锁添加一个属性来记录其最大优先级`max_priority_in_query_threads`（在请求锁的线程中的最大优先级，以下统称"锁的最大优先级"），并初始化为`PRI_MIN`；另一方面，一个线程可能持有多个锁，当该线程释放掉其中一个锁之后，其当前向外表现出来的`priority`不能立刻恢复到`init_priority`而是下降到当前持有锁列表中的其他锁的最高优先级，出于对该需求的考虑，应该在线程中添加一个属性`locks_possess_list`来记录持有锁列表，同时为了方便线程释放锁后更新当前的优先级，因此可以将其设计为优先级队列。
3. 最后，针对第二个要求即多层捐献而言，当高优先级线程 H 找到中优先级线程 M 时，必须要能够知道 M 是否正在被另一个锁所卡住以及那个锁和持有锁的线程的信息，所以需要在`thread`结构体中添加一个指针`await_lock`来记录该锁。

&emsp;对于第一个阶段和第二个阶段的实现主要体现在申请锁`lock_acquire`和释放锁`lock_release`函数中。当线程欲申请锁时，在获得前和获得后都需要进行一系列处理。在获得锁之前需要判断当前锁是否被其他线程所持有，如果被持有，那么就需要开始考虑捐献的情况，同时设置`await_lock`为该锁，若该锁的最大优先级低于当前线程，那么就更新该锁的最大优先级为当前线程的优先级。同时由于锁的优先级改变了，那么对于锁的持有者来说需要判断该锁优先级的改变是否会影响到它的优先级（因为持有者不一定只持有这一个锁，故并不能简单的就更新持有者的优先级），若当前线程的优先级仍然大于锁的持有者的优先级，那么才发生捐献情况。总结来说，捐献一方面是对锁的捐献，另一方面是对线程的捐献，后者依赖于一系列前者的变化。在获得锁之后，那么就需要将该锁按照优先级次序加入到该线程的持有锁列表中并更新`await_lock`表明此时已经没有需要等待的锁了。相应的，当线程释放锁时，也不是简单地将线程的优先级恢复为`init_priority`而是取其他锁中的最大优先级来更新，若持有锁列表为空或最大优先级低于原始优先级才会执行恢复。同时与释放锁类似的是`thread_set_priority`对优先级的更新，首先无论如何都更新`init_priority`其次只有在新优先级比此时表现的优先级更大时候，才更新`priority`。同时由于释放锁的时候需要用到`sema_up`函数，其对应的是的`sema_down`函数（在线程成功请求锁的时候调用，如果锁已被持有会将该线程添加到该信号量的等待线程中并阻塞），因此在释放锁的时候也应该遵循优先级调度原则，将信号量的等待队列`waiters`中的最大优先级线程释放出来。同理，对于条件变量而言，同样需要在`cond_signal`中释放具有更高优先级的信号量中的等待线程（因为对于一个管程而言其条件变量`cond`在等待多个信号量的事件触发）。究其根本，如果在加入某一队列的时候没有按照优先级次序添加，那么取出的时候就必须按照线性遍历的方式获取最高优先级的线程。

&emsp;完成以上基本过程后足以应对单级捐献的需求，以下考虑多级捐献问题。多级捐献发生的本质即在当前线程申请锁时判断该锁是否有持有者，若有持有者则判断该持有者是否还在等待别的锁即可以简单地通过`await_lock`来判断。因此在该背景下需要循环或者递归进行优先级捐赠，我采用`while`循环的方式完成多级捐赠。首先规定一个最大深度来避免处理深度过大导致堆栈溢出等情况的发生，其次如果锁的持有者`lock_holder`所等待的锁`lock_next`的持有者即`lock_holder_next`的优先级更大那么就直接退出循环即可，否则就更新`lock_holder_next`的最大优先级,至于`lock_next`的优先级不论是否退出循环都需要更新。我认为这里有一个很关键的点，即对于单级捐献而言，不仅需要更新锁的最大优先级还需要更新锁的持有者的最大优先级，因为当前线程是作为该锁的请求者，同时为了能够让该持有者能够被调度所以也需要对它进行捐献，两者先后没有影响；但是对于多级捐献而言，`lock_next`的请求者是`lock_holder`因此对于`lock_next`的优先级更新应该早于`lock_holder_next`,我们可以假设，如果先判断对`lock_holder_next`的更新，假设此时`lock_holder`的优先级比它低，那么就直接将循环`break`了，这是不能接受的，因为`lock_holder_next`的优先级是由它所持有的的所有锁的最大优先级决定的，即使`lock_holder`的优先级小于它也并不意味着小于`lock_next`，因此不该跳过`lock_next`的更新，在代码实现的时候需要将锁的更新先于线程的更新。这一点在上文中也提到过

> 总结来说，捐献一方面是对锁的捐献，另一方面是对线程的捐献，后者依赖于一系列前者的变化。

所以，我们应该先更新锁的优先级再更新线程的优先级，同样的可以总结发现单级捐献不考虑先后也可以统一为先更新锁的优先级再更新线程的优先级。

### Synchronization

&emsp;由于本专题的主题是优先级调度，因此本身是基于多线程并发环境的，所以我们应该对修改代码后是否影响多线程并发环境下的同步进行分析。经过分析可以归纳出涉及同步代码的函数主要有：

1. `sleep_thread`在将当前线程加入到睡眠线程队列中后调用`thread_block`，而在阻塞前后需要分别进行关中断（阻塞函数中有对中断等级为`INTR_OFF`的断言）和中断恢复；
2. `sleep_thread_list_tick`在睡眠线程的`ticks`数小于等于零时进行线程唤醒调用`thread_unblock`，该函数在将当前线程加入就绪队列并修改线程状态前后分别进行关中断和中断恢复，若唤醒的线程优先级高于当前线程则调用`intr_yield_on_return`该函数将`yield_on_return`变量置为`true`那么中断处理函数返回前就会调用`thread_yield`,该函数在将当前线程加入就绪队列并进行重新调度前后分别进行关中断和中断恢复；
3. `thread_set_priority`在新优先级大于当前优先级时会调用`thread_yield`，与上文同理；

&emsp;以上的同步主要基于 Disabling Interrupts（禁用中断）来实现，禁用中断同时也是 Semaphores（信号量）和 Locks（锁）以及 Monitors（管程）的基础：

1. 线程申请锁时调用`lock_acquire`，该函数在获取锁时调用`sema_down`并为该函数传入该锁的信号量，如果不满足条件那么就将当前线程放入信号量的阻塞队列`sema->waiters`中；
2. 线程释放锁时调用`lock_release`，该函数调用`sema_up`并为该函数传入该锁的信号量，同时在有需要的情况下唤醒`sema->waiters`中的阻塞队列；
3. 在使用管程作为同步方式的时候，调用`cond_wait`函数，该函数通过`lock_release`释放锁、`sema_down`将当前线程放入`waiter`信号量的阻塞队列中，`lock_acquire`重新获得锁。监视器本身由锁和一个或多个“条件变量”组成。而在调用`cond_signal`的时候，则将条件变量的等待信号量队列中的具有较大优先级的信号量中的等待队列中的线程唤醒。  
   &emsp;经分析可以发现本专题所修改的函数已经囊括在上述的分析中，并未修改核心的同步部分，因此并不会破坏同步功能，Pintos 系统可以完成多线程并发环境下的同步。

### Rationale

&emsp;首先是对`thread_yield`和`thread_unblock`函数修改的说明，我选择在前两者中将`list_push_back`修改为`list_insert_ordered`而不是将`schedule`函数所调用的`next_thread_to_run`进行修改，这主要是因为在需要多次调度的时候，修改入端即在插入`ready_list`的时候保持按优先级降序排序比修改出端即在选择下一个线程进行调度的时候遍历就绪队列取出优先级最高的线程，修改入端的效率显然是更加高效的，尤其是在`list.c`已经为我们提供了`list_insert_ordered`函数时。
&emsp;其次是对多级捐献中采用循环而不是采用递归的说明，由于 Pintos 中线程的堆栈一般只有有限的地址空间，而递归在深度较大的情况下每一次都需要保存局部变量、形参、调用函数的地址和返回值，因此会更有可能造成栈溢出的情况同时会影响到执行效率，相对而言在循环能解决的前提下循环的执行效率更高结构更加简单。

## Mission 3 Advanced Scheduler

### Data Structures

新建或修改的 struct 或 struct 成员、全局或静态变量、typedef 或枚举的声明

```c
static int f = 16384; //参考pintos中BSD调度p.q定点格式来以定点数定义实数其中p=14，q=17故f=2**14
typedef int fixed_point;//为int取别名fixed_point用于将int类型的值转换为定点数
#include "fixed-point.h"//在`thread.h`中引入fixed-point头文件
struct thread
  {
    ...
    fixed_point recent_cpu;//线程最近使用的CPU的时间的估计值
    int nice;//每个线程都有一个整数nice值该值确定该线程与其他线程应该有多“不错”[-20,20]
  };
  #include "timer.h"//在`thread.c`中引入`time.h`来获取当前的ticks
  static fixed_point load_avg;//在`thread.c`中定义全局静态定点数变量
```

新建或修改的函数

```c
//将int类型的值转换为fixed_point
static inline fixed_point itof(int n);
//方法1：将fixed_point类型的值转换为int
static inline int ftoi(fixed_point x);
//方法2：将fixed_point类型的值转换为int
static inline int ftoi_round(fixed_point x);
//将fixed_point类型的数与int类型的数相加
static inline fixed_point add_fi(fixed_point x, int n);
//将fixed_point类型的数与int类型的数相减
static inline fixed_point sub_fi(fixed_point x, int n);
//将fixed_point类型的数与fixed_point类型的数相乘
static inline fixed_point multiply_ff(fixed_point x, fixed_point y);
//将fixed_point类型的数与fixed_point类型的数相除
static inline fixed_point divide_ff(fixed_point x, fixed_point y);
//针对每秒更新一次load_avg的需求编写对应的函数
//计算表达式中的系数并获得就绪队列中的进程数量同时判断此时正在运行的线程是否是idle_list
//按照pintos文档中的需求，需要将正在运行的线程也算在ready_threads中
static void update_load_avg_mlfqs();
//为高级调度程序初始化相关数据
static void init_thread (struct thread *t, const char *name, int priority);
//针对每秒更新一次线程最近使用CPU时间的需求编写对应的函数
//在文档中提示我们建议先计算recent_cpu的系数然后再相乘否则load_avg和recent_cpu直接相乘会导致溢出
static void update_recent_cpu_mlfqs(struct thread* t,void *aux UNUSED );
//针对每四秒更新一次线程的优先级编写对于的函数
//需要注意的是计算完之后需要和PRI_MAX以及PRI_MIN进行比较
static void update_priority_mlfqs(struct thread* t,void *aux UNUSED);
//为了适应高级调度的需求需要对thread_mlfq进行判断
//如果当前值为true那么就获取当前的ticks并将当前运行的非闲置进程的最近CPU使用时间增加
//每四秒更新一次线程的优先级
//每秒更新一次load_avg（系统平均负载）并根据load_avg来计算当前的recent_cpu时间
void thread_tick (void);
//将当前正在运行的线程的nice值设置为`nice`
void thread_set_nice (int nice UNUSED);
//返回当前正在运行的线程的nice值
int thread_get_nice (void);
//返回100倍的系统平均负载
int thread_get_load_avg (void);
//返回100倍的当前正在运行的线程的当前CPU运行时间
int thread_get_recent_cpu (void);
//初始化load_avg全局变量
void thread_start (void);
//在mlfqs模式下需要忽略该函数
void thread_set_priority (int new_priority)
//在mlfqs模式下不需要进行优先级捐赠
void lock_acquire (struct lock *lock)
//在mlfqs模式下不需要进行优先级捐赠
void lock_release (struct lock *lock)
```

### Algorithms

&emsp;本专题针对的是多级反馈队列（Multi-Level Feedback Queue，MLFQ），其背景为通用调度程序的目标是平衡线程的不同调度需求。那些执行大量 I/O 操作的线程需要快速的响应时间，以保持输入和输出设备繁忙，但只需很少的 CPU 时间。另一方面，计算密集的线程需要接收大量 CPU 时间完成计算，但无需快速的响应时间。其他线程介于两者之间，I/O 周期和计算周期交替执行，因此随着时间变化会有不同需求。设计良好的调度程序通常可以接纳所有不同要求类型的线程。线程优先级由调度程序使用下面给出的公式动态确定。但是，每个线程还有一个整数 nice 值，该值确定该线程与其他线程应该有多“不错”。nice 为零不会影响线程优先级。正的 nice（最大值为 20）会降低线程的优先级，并导致该线程放弃原本可以接收的 CPU 时间。另一方面，负的 nice，最小为-20，往往会占用其他线程的 CPU 时间。初始线程以 nice 值为零开始。其他线程以从其父线程继承的 nice 值开始。
&emsp;我们的调度程序具有 64 个优先级，因此有 64 个就绪队列，编号为 0（PRI_MIN）到 63（PRI_MAX）。较低的数字对应较低的优先级，因此优先级 0 是最低优先级，优先级 63 是最高优先级。线程优先级最初是在线程初始化时计算的。**每个线程的第四个时钟滴答也会重新计算一次**。无论哪种情况，均由以下公式确定：

$$
priority=PRI\_MAX-\frac{recent\_cpu}{4}-2*nice(公式1)
$$

其中 recent_cpu 是线程最近使用的 CPU 时间的估计值，而 nice 是线程的 nice 值。结果应四舍五入到最接近的整数（舍去）。该公式把最近获得 CPU 时间的线程赋予较低的优先级，以便在下次调度程序运行时重新分配 CPU。这是防止饥饿的关键：最近未获得任何 CPU 时间的线程的 recent_cpu 为 0，除非高 nice 值，否则它很快会获得 CPU 时间。
&emsp;我们使用指数加权移动平均值来计算 recent_cpu 参数，其一般形式为:

$$
x(0) = f(0)
$$

$$
x(t) = a*x(t-1) + f(t)
$$

$$
a = \frac{k}{k+1}
$$

因此我们可以用该一般形式的公式推出 recent_cpu 在该专题中的计算方式

$$
recent\_cpu =\frac{2*load\_avg}{2*load\_avg + 1}  * recent\_cpu + nice（公式2）
$$

某些测试所作的假设要求，当**系统刻度计数器达到一秒的倍数时(时间周期为一秒)**，即，当`timer_ticks()%TIMER_FREQ == 0 `时，准确地进行 recent_cpu 的重新计算。而不是其他任何时间。
&emsp;最后，load_avg（通常称为系统平均负载）估计过去一分钟准备运行的线程的平均值。与 recent_cpu 一样，它是指数加权的移动平均值。与 priority 和 recent_cpu 不同，load_avg 是系统级的，而不是特定于线程的。在系统引导时，它将初始化为 0。此后**每秒一次（时间周期为一秒）**，将根据以下公式进行更新：

$$
load\_avg = \frac{60}{59}*load\_avg + \frac{1}{60}*ready\_threads（公式3）
$$

最终，根据公式 1~3 即可计算出 load_avg 的值以及当前线程 recent_cpu 的值并根据这两个值计算出当前线程的优先级。MLFQS 为了计算 priority 引入了 nice，recent_cpu 和 load_avg 的概念，后两者为实数。然而 Pintos 内核中不支持浮点数类型，所以文档中为我们介绍了一种用整型模拟浮点数的方法 fixed-point，将整型的一部分位拿来表示小数部分（位数的分配自行规定，文档中介绍的是 17.14 格式）。添加一个头文件 fixed_point.h，因为其实现相对比较简单因此函数就直接在头文件里面定义了。其中 fixed_point 之间的加减法、fixed_point 和 int 的乘除法无需特殊处理。

---

&emsp;在实践的过程中，基于上述背景首先需要考虑的是在`thread.h`中为`struct thread`添加对应的属性即`nice`和`recent_cpu`同时在`thread.c`中添加全局静态变量用于标识系统平均负载`load_avg`，针对这一部分的内容主要需要考虑的是在线程创建的时候即调用`thread_create`函数时能够将`nice`和`recent_cpu`初始化为 0 同时由于`load_avg`是系统级的变量而不是线程级的因此需要在`thread_start`中进行初始化，该函数是在创建所有的线程之前执行的。此外必须考虑的是默认情况下优先级调度程序必须处于活动状态但是我们必须能够通过`-mlfqs`内核选项来选择 BSD 调度算法因此需要对所编写的程序必须的位置处对`thread_mlfqs`进行必要的判断，当启动该算法后同时应该注意的是需要忽略对`thread_create()`的优先级参数（这里忽略的意思可以简单理解为将优先级设置为 0，使得实际的优先级由后续的`update_priority_mlfqs`函数来计算）以及对`thread_set_priority`的任何调用，而对`thread_get_priority`的调用应该返回调度程序设置的线程的当前优先级，同时，高级调度程序不会进行优先级捐赠，这也是需要利用`thread_mlfqs`来进行控制的。
&emsp;本专题中隐藏的细节即需要我们对`synch.c`文件中的`lock_acquire`函数和`lock_release`函数中关于优先级捐赠相关的部分进行判断，这一步十分关键，本人一开始以为并不需要特别注意该部分，导致的结果就是在多线程并发的环境下出现巨大的问题。

### Synchronization

&emsp;该专题的同步功能主要分两个方面来说明，首先是基于多级反馈队列的优先级算法，通过

$$
priority=PRI\_MAX-\frac{recent\_cpu}{4}-2*nice
$$

在每四个时钟滴答的时间更新每个线程的优先级大小，这样就能保证每个线程在运行的过程中优先级是动态变化的，那么当正在运行的线程运行时间达到预定的时间片后加入就绪队列同时再从就绪队列中取一个最大优先级的线程时就能够在一定程度上保证各个线程之间不会发生某个线程永远分配不到 CPU 的情况即饥饿，同时并发的执行之间也能够通过时间片来轮询，这又保证了并发的需求。
&emsp;其次是对于优先级调度的问题的补充，即需要避免掉优先级捐赠，这是本人在不断的测试过程中发现的问题，如果不在`synch.c`中将`lock_acquire`和`lock_release`函数中关于优先级捐赠的部分去除，那么将在`thread_init`中分配`tid`的时候前一个线程的优先级随着时间的运行会减少，但是由于是并发环境那么当后续线程申请锁的时候就会将其相对较高的优先级捐赠给前一个获得锁的线程，于是导致不断发生捐赠导致执行时间被拉长。

### Rationale

&emsp;这一部分由于需求相对较为明确，因此主体思路不同人实现的大同小异，我主要是对某些过程进行了一点小的优化，比如`fixed-point.h`中并不将所有的情况都封装成函数而是选择将需要进行转换的运算封装成函数，这样在实际的使用过程中可能会减少用户栈的堆积加快执行速度，同时将`load_avg`、`recent_cpu`、`priority`的计算封装成三个函数来使得程序的结构相对更加清晰。此外，值得一提的是在计算时的系数问题，优先考虑将系数先计算再与别的数相乘而不是将分子与别的数相乘后再进行定点数计算，这样的解决办法可以有效避免数值溢出的情况。

# Project 2：User Programs

## Background

&emsp;本人在运行`pintos-mkdisk filesys.dsk --filesys-size=2`创建具有文件系统分区的模拟磁盘后，尝试运行`pintos -f -q`在内核命令行上传递参数来格式化文件系统分区时发生了三个故障：

1. 其中第一个故障大致意思是“Pintos 无法解析参数-f 经过查询发现是在 Pintos 的环境配置过程中`utils/pintos`中用户程序例程加载的内核路径没有更改，还是加载的`threads/build/kernel.bin`，修改为`userprog/build/kernel.bin`即可；
2. 第二个故障是在`userprog`文件夹编译后无论如何运行 pintos 相关的命令都会立刻发生`kernel panic`，在网上有类似的两个博客记录了类似的问题，追踪回调地址会可以还原问题的发生：系统启动`pintos_init()`函数首先会去调用`thread_init()`函数，其中完成初始化后会去分配`tid`，在分配的过程中需要涉及到锁的申请和释放，因此在释放锁的时候会调用`thread_schedule_tail()`函数，由于本专题具有`USERPROG`的宏定义因此在调用`process_activate()`时后者最终调用`pagedir_activate()`其中传入的参数是线程的`pagedir`成员，但是此时还并未对线程进行重新构造于是在函数中`pd`就被初始化为`init_page_dir`而该全局变量必须在`paging_init`中才会被初始化而好巧不巧的是该函数在`thread_init`之后被调用，所以此时的`init_page_dir`也是`0x0`不属于有效的内核地址于是虚拟地址会向物理地址转换的`vtop`函数断言失败，解决办法是在`thread.c`文件中定义全局变量`ready_to_schedule`并在`thread_start()`中才设置为`true`最为关键的是在`thread_yield()`函数中添加对该参数的判断才继续执行，确保不发生上述的错误；
   > 该解决方案的设想主要是基于`thread_start`在函数`thread_init`之后执行
3. 第三个故障则是在判断不属于中断上下文的断言出了问题，是在`idle_init`中产生的追踪发生最终是在`sema_up`中进行了调度，因此需要在`sema_up`的调度前增加中断上下文的判断；

&emsp;值得一提的是如果在完成 Project1 的基础上对 Project2 进行开发那么还将解决的另一个很隐蔽的问题是环境的配置，即在配置环境的时候，我们已经将`/utils/Pintos.pm`中的`loader`初始化为`threads/build`下的`loader.bin`同理也将`/utils/pintos`中的`kernel.bin`初始化为`threads/build`下的`kernel.bin`，因此需要分别替换为`userprog/build`下面的内容，否则可能会出现无内核或者文件系统无法加载或者读取的错误，同样的将这一部分内容修改后需要在`util`中执行`make`操作，注意此时再回到 Project1 的`threads/build`下执行`make clean&&make&&make check`将会出错，因为环境已经更改了，因此如果不具备调优能力，那么建议从**初始化**的`Pintos`中`fork`一个新分支或者写完 `Project1` 后用`git`保存当前版本库以便在后续出错后能够进行版本回退，但是如果具备比较清晰的解决思路，那么采用宏定义即`#ifdef`配合`#endif`来实现**条件编译**是最优的选项，我在此处便是采用该方法使得本地能够兼容`Project1`以及`Project2`。但是需要注意的是兼容并不代表能在本地同时对两个项目执行`check`操作，这依赖于特定的环境，而在学校提供的测试平台中对于源代码而言会为每个`Project`提供单独的测试环境因此在服务器端可以实现分别对两个项目执行`check`，这便是兼容的意义。对于`Project4`而言同样如此，兼容使得最终在服务器上只需要保留一套完整的代码即可分别通过不同`Project`下的测试。
&emsp;下面简单介绍一下用户程序的加载流程：
![用户程序的加载流程](pic/%E7%94%A8%E6%88%B7%E7%A8%8B%E5%BA%8F%E7%9A%84%E5%8A%A0%E8%BD%BD%E6%B5%81%E7%A8%8B.png)

<center>用户程序的加载流程</center>

`Pintos`在运行的过程中会首先执行`main`函数在其中对各种内核属性和用户属性以及相关的参数进行初始化后执行`run_actions`函数，在其中会根据传入的参数选择不同的`action`并执行对应的函数同时传递对应的参数（但是在一开始参数是作为一个整体传递的，我们后续要做的任务之一就是进行参数解析），如果上述`action`是`run`那么就会转到执行`run_task`函数，在该函数中如果是用户程序那么就会将整个字符串作为参数传入`process_execute`函数中，在该函数中我们根据要求解析得到用户可执行文件的名称并作为线程名来创建线程，而线程的程序通过`start_process`来执行，效果是创建该线程而该线程会去执行`start_process`，在该函数中内核根据用户可执行文件的名称（我们来解析参数得到的）来`load`文件系统中的可执行文件来执行，而在该函数中首先是打开该文件执行，其次是为该程序构造一个用户栈来存储真正的参数，以上图为例就是执行`echo`程序并传入`x`参数，这就是典型的用户程序的执行过程。
&emsp;其次简单介绍以下用户程序的系统调用流程：
![用户程序系统调用流程](pic/%E7%94%A8%E6%88%B7%E7%A8%8B%E5%BA%8F%E7%9A%84%E7%B3%BB%E7%BB%9F%E8%B0%83%E7%94%A8%E6%B5%81%E7%A8%8B.png)

<center>用户程序系统调用流程</center>

用户程序在其`main`函数中欲调用系统调用时，例如`OPEN`,那么就会通过预定义好的`syscall(open,file)`接口来调用，此时系统调用号`open`和参数`file`被压到调用者的栈中，而在`syscall`函数本身其通过使用 `int $0x30 `指令调用系统调用的中断此时进入到中断处理程序`intr_handler`在其中根据前述指令`int $0x30`判断该中断是系统调用中断，因此转到`syscall_handler`函数中，在该函数中我们实现将各种类型的系统调用进行分配，比如通过`switch`来将`open`分配到我们自己定义好的`syscall_open`函数中即可完成一次系统调用。

## Mission 1 Process Termination Messages

### Data Structures

新建或修改的 struct 或 struct 成员、全局或静态变量、typedef 或枚举的声明

```c
无
```

新建或修改的函数

```c
//每当用户进程终止时打印该进程的名称和退出代码
void process_exit (void)
```

### Algorithms

> 任务需求为：每当用户进程终止时，不管是因为它调用 exit 或任何其他原因，请打印该进程的名称和退出代码，打印的名称应为传递给 process_execute（）的全名，并省略命令行参数。当不是用户进程的内核线程终止时，或者在调用“halt”系统调用时，请勿打印这些消息。当进程无法加载时，此消息是可选的。

根据该需求可以简单分析发现所有程序的退出包含在`thread_exit`内当且仅当运行的是用户程序（即具有`USERPROG`的宏的时候）才会调用`process_exit`，因此可以执行该函数时打印相关的信息，而不是在别处，避免打印内核线程的退出信息。

### Synchronization

&emsp;无特别的同步需求

### Rationale

&emsp;主要是解决当不是用户进程的内核线程终止或者是因为`halt`系统调用时候不打印这些信息

## Mission 2 Argument Passing

### Data Structures

新建或修改的 struct 或 struct 成员、全局或静态变量、typedef 或枚举的声明

```c
无
```

新建或修改的函数

```c
//为FILE_NAME生成两份拷贝，其中一份用于线程名，另一份用于start_process的参数
//目的主要是为了避免发生访存冲突
//通过Pintos文档提示的strtok_r函数来分割字符串
//将分割后的字符串用于线程名称的赋值，将另一份拷贝用于start_process进行处理
tid_t process_execute (const char *file_name)
//以文件名加载一个ELF格式的可执行文件并将统治权转交给该程序同时传递正确的参数
//记录一个错误：在将参数压入栈之前需要格外注意的是不能提前释放空间！！
static void start_process (void *file_name_)
```

### Algorithms

&emsp;首先考虑的是在`process_execute`函数中进行的更改，针对`thread_create`而言，需要将传递的参数`file_name`首先分割出`process_name`即第一个命令行字符串作为线程的名称，其次需要考虑的是将原来完整的`file_name`作为参数传递给`start_process`函数，这里需要注意的是不能简单将原来的`file_name`传递，而是需要形成一份拷贝之后再进行传递否则将会造成参数缺失。同样需要考虑的是**内存释放**。
&emsp;其次需要重点关注的是`start_process`函数，需要做的是在程序`load`完文件系统中的可执行文件时将对应的参数压入`esp`中：

- 首先将栈指针指向用户虚拟地址空间的开头即（PHYS_BASE）。
- 其次解析参数，将它们放置在栈顶并记录他们的地址，此处单词的顺序无关紧要，因为此时要做的是将单词的地址记录在`argv`中，此后只需要按照记录的顺序来“解码”即可。
- 字对齐的访问比未对齐的访问要快，因此为了获得最佳性能，在第一次压入之前将堆栈指针向下舍入为 4 的倍数。
- 推送每个字符串的地址加上堆栈上的空指针哨兵，按从右到左的顺序。
- 将 argv 的地址即 argv[0]压入栈中，使得程序能够在后续访问到上述参数
- 将 argc 压入栈中，使得程序能够在后续访问到参数的个数
- 推送一个伪造的“返回地址”：尽管入口函数将永远不会返回，但其堆栈框架必须具有与其他任何结构相同的结构。
  最后在`start_process`中跳转到对应的位置为加载的程序提供参数

### Synchronization

&emsp;无特别的同步需求

### Rationale

&emsp;唯一需要注意的就是在`process_execute`中传入的实参需要进行拷贝，避免用同一个地址空间来适应同一个字符串需要做两件事的需求，一个用来获得文件名称（线程名称）另一个用来传递参数。

## Mission 3 System Calls

### Data Structures

新建或修改的 struct 或 struct 成员、全局或静态变量、typedef 或枚举的声明

```c
struct thread{
  ...
  int exit_code;//线程的退出状态
  struct thread *parent;              //该进程的父进程
  struct list child_list;                 //该进程的子进程列表，每一个元素为child_entry类型的
  struct child_entry *as_child;       //该进程本身作为子进程时维护的结构
  struct semaphore exec_sema;//用于父进程等待成功加载子进程的可执行文件时的阻塞
  bool exec_success;//判断加载是否成功
}
//子进程的全部信息：不能仅存储子进程本身，而是需要将其中关键的信息也抽取出来
struct child_entry
{
  tid_t tid;                          //子进程的tid
  struct thread *t;                   //子进程本身的指针，当它不是活动状态时设置为NULL
  bool is_alive;                      //判断子进程是否为活动状态
  int exit_code;                      //子进程的退出状态
  bool is_waiting_on;                 //判断是否一个父进程正在等待该子进程
  struct semaphore wait_sema;         //用于同步父进程对子进程的等待
  struct list_elem elem;
};

//在'filesys.h'中定义：对文件系统加锁
struct lock filesys_lock;
//某个线程所打开的某个文件的管理结构
struct file_entry
{
  int fd;                             /**< File descriptor. */
  struct file *f;                     /**< Pointer to file. */
  struct list_elem elem;
};
```

新建或修改的函数

```c
//在用户的中断处理程序中解析系统调用的符号
//根据类型分发给各个具体处理函数逐个实现
static void syscall_handler (struct intr_frame *f UNUSED);
//强制退出Pintos`halt`
static void syscall_halt(struct intr_frame *f UNUSED)
//为线程初始化退出状态exit_code
//初始化exec的等待信号量和相应的bool属性
//初始化文件管理列表和下一个被分配到的文件描述符
static void init_thread (struct thread *t, const char *name,int priority)
//添加忙等待避免直接退出
int process_wait (tid_t child_tid UNUSED)
//根据tid来终结某个进程
int thread_dead(tid_t tid);
//为父子进程初始化子进程列表
static void init_thread (struct thread *t, const char *name, int priority)
 //为父子进程相关的结构和其参数初始化
 //将该进程与创建该进程的父进程相链接
tid_t thread_create (const char *name, int priority,thread_func *function, void *aux)
//查找自身的 child_list，如果不存在目标子线程，返回 -1；如果存在且子线程未结束，也没有 wait 过，用信号量卡住自身等待子线程运行结束；如果 wait 过，返回 -1；如果已经结束了，返回子线程留下来的 exit_code。
int process_wait (tid_t child_tid)
//综合考虑某个进程退出时需要既考虑其作为父进程的身份，也需要考虑其作为子进程的身份
//关闭该线程所打开的所有文件
void thread_exit (void)
//运行其名称在 cmd_line 中给出的可执行文件，并传递任何给定的参数，返回新进程的进程ID(pid)
static void  syscall_exec(struct intr_frame *f)
//如果子进程没有正常运行起来，父进程要返回 -1。
//子进程的可执行程序可能因为找不到文件等原因而加载失败时让父进程等待：
tid_t process_execute (const char *file_name)
//如果执行失败就将子进程的状态设置为结束状态同时设置状态码为-1并唤醒父进程
//如果执行成功就将父进程的执行成功标识符置为true并唤醒父进程
static void start_process (void *file_name_)
//从用户虚拟地址空间中读取一个字节的信息，如果成功那么就返回该信息否则返回-1
static int get_user (const uint8_t *uaddr);
//向用户地址空间写入一字节的信息如果成功就返回true否则返回false
static bool put_user (uint8_t *udst, uint8_t byte
//检查一个用户提供的指针是否能够合法读取数据，如果合法就返回该指针否则就调用terminate_process
static void* check_read_user_ptr(const void *ptr, size_t size)
//检查一个用户提供的指针是否能够合法写数据，如果合法就返回该指针否则就调用terminate_process
static void* check_write_user_ptr(void *ptr, size_t size)
//检查一个用户提供的字符串是否能够合法写数据，如果合法就返回该字符串否则就调用terminate_process
static char* check_read_user_str(const char *str)
//终止一个进程
static void  terminate_process(void));
//在对user变量赋值后判断是否是由syscall引起的错误
static void page_fault (struct intr_frame *f)
//对于非syscall的用户态错误需要将退出码置为-1
static void kill (struct intr_frame *f)
//初始化文件系统的访问锁
void filesys_init (bool format)
//创建一个名为file的新文件，其初始大小为 initial_size个字节。如果成功，则返回true，否则返回false。创建新文件不会打开它：打开新文件是一项单独的操作，需要系统调用“open”。
static void syscall_create(struct intr_frame *f)
//打开名为 file 的文件， 返回一个称为"文件描述符"(fd)的非负整数句柄；如果无法打开文件，则返回-1.
static void syscall_open(struct intr_frame *f)
//根据fd来查找当前线程是否管理着文件描述符为fd的文件，如果存在那么返回file_entry*否则就返回NULL
static struct file_entry *get_file_by_fd(int fd)
//返回以fd打开的文件的大小（以字节为单位）
static void syscall_filesize(struct intr_frame *f)
//从打开为fd的文件中读取size个字节到buffer中。返回实际读取的字节数（文件末尾为0），如果无法读取文件（由于文件末尾以外的条件），则返回-1。 fd 0使用input_getc()从键盘读取。
static void syscall_read(struct intr_frame *f)
//适应写调用：将size个字节从buffer写入打开的文件fd返回实际读取的字节数（文件末尾为0）
//如果无法读取文件（由于文件末尾以外的条件），则返回-1；fd 0使用input_getc()从键盘读取
static void syscall_write(struct intr_frame *f)
//将打开文件fd中要读取或写入的下一个字节更改为position，以从文件开头开始的字节表示。（因此，position为0是文件的开始。）
static void syscall_seek(struct intr_frame *f)
//删除名为file的文件。如果成功，则返回true，否则返回false。不论文件是打开还是关闭，都可以将其删除，并且删除打开的文件不会将其关闭。
static void syscall_remove(struct intr_frame *f)
```

### Algorithms

该部分相对来说较为复杂，可以分成多个模块来叙述

#### Halt+Exit+Write(部分)

&emsp;Halt：通过调用`shutdown_power_off()`(在`threads/init.h`中声明)终止 Pintos。很少使用此方法，因为可能会丢失一些有关可能的死锁情况的信息等等。为简单通过测试，这里就直接采用`shutdown_power_off()`了。
&emsp;Exit:终止当前用户程序，将 status 返回到内核。如果进程的父进程等待它，则将返回此状态，即在`as_child`的结构体中保留下退出码并唤醒父进程。
&emsp;Write;将 size 个字节从 buffer 写入打开的文件 fd。返回实际写入的字节数，如果某些字节无法写入，则可能小于 size。在文件末尾写入通常会扩展文件，但是基本文件系统无法实现文件增长。预期的行为是在文件末尾写入尽可能多的字节并返回实际写入的数字，如果根本无法写入任何字节，则返回 0。在此部分我主要是先实现了`fd==1`的情况即将`buf`缓冲区的内容写入到控制台中，使用`putbuf`是合理的，否则，不同进程输出的文本行可能最终会在控制台上交错出现。

#### Wait+EXEC+父子进程问题的解决

&emsp;Wait：该系统调用不只是简单的等待，而是涉及连锁的父子进程等问题，我们在设计算法的时候根据文档的描述可以归纳出一套基本的流程顺序修改`process_wait`：

1. 遍历当前进程的所有子进程
2. 如果存在进程所需要等待的子进程 tid，则继续从(2.3.4.5)中进行判断
3. 如果该子进程并没有被父进程所等待同时子进程并没有结束那么就使用信号量卡住自身来等待子进程运行结束
4. 如果已经在等待该子进程了那么就返回-1
5. 如果子进程已经结束那么就返回 exit_code
6. 上述条件都不满足就认为该线程没有`tid`为参数所表征的子线程或者该线程已经被内核中断了，于是返回-1

同时为了与等待相呼应，需要对`thread_exit`进行对应的修改：

1. 作为一个父进程
   - 遍历当前进程（即企图结束的进程）的子进程表并通知所有存活的子进程自己已经结束
2. 作为一个子进程
   - 如果其父进程已经终结那么其维护 as_child 已经没有意义，于是可以释放
   - 如果还未终结，那么由于它自身已经终止那么需要把退出码保存到 as_child 结构
     - 同时如果存在父进程在等待自己，那么就将父进程从阻塞队列中调出
   - 更新其作为子进程结构的信息（当前线程设置为 NULL、存活设置为 False）

&emsp;Exec：运行其名称在`cmd_line`中给出的可执行文件，并传递任何给定的参数，返回新进程的进程 ID(pid)。 如果程序由于任何原因无法加载或运行，则必须返回 pid -1，否则不应为有效 pid。 因此，父进程在知道子进程是否成功加载其可执行文件之前不能从 exec 返回。于是我通过在`process_execute`的`thread_create`后使得当前父线程被阻塞，直到在`start_process`中子进程正确加载才唤醒父进程继续执行。

#### 用户访存检查

&emsp;文档提示我们至少有两种解决方案：

> 第一种方法是验证用户提供的指针的有效性，然后解引用它。可以理解为地址是否属于用户内存区域(小于 `PHYS_BASE`)以及地址是否属于当前进程的内存区域。

> 第二种方法是仅检查用户指针是否指向`PHYS_BASE`下方，然后解引用。可以理解为仅做前者的检查然后就访问，如果不合法会引发 page fault，然后再处理这个异常。

其中第二种方法通常更快，因为它利用了处理器的 MMU,因此倾向于在实际内核（包括 Linux）中使用，本文也是基于该方法来实现的访存检查；因此首先我们将讲义提供的两个函数写到`syscall.c`中方便调用，并为其写出`check_xx_user_p/str`的访存检查函数，如果发生访存错误，那么就通过中断处理程序跳转到`exception.c`中的`page_fault`内进行处理，对于内核态的代码如果没有明显的逻辑错误是不会进入页面错误中断的，于是可以简单的认为如果在内核态发生了页面错误那就是`syscall`导致的，所以简单通过`user`局部变量即可判断是否是系统调用，如果是就将 `EAX `的值拷贝到`EIP`中，然后将`EXA`设为`0xffffffff（-1）`;同时需要注意的是如果异常是由用户自身引起的，比如说直接访问`NULL`那么将会进入`kill`函数的`SEL_UCSEG`分支，此时我们需要将线程的退出状态设置为`-1`;最终在`syscall.c`中完成访存检查，如果发现访存错误那么就直接终止线程并将线程的退出码设置为-1，需要注意的是字符串和普通指针不一样，在访存检查的时候还有末尾的`\0`需要注意所以需要专门为字符串的检查也定义一个函数，同时由于某些指针的内容本身也是指针（如字符串）那么对于解引用后的内容还需要再次执行访存检查。

#### 文件系统调用包括 CREATE+REMOVE+OPEN&&其他剩余的系统调用

&emsp;这一部分我们需要做的是要是对线程文件资源进行管理，具体的文件操作只需要调用`Pintos`封装好的函数即可，在`filesys.h`和`file.h`中，甚至讲义上提到的一些特殊要求，例如打开的文件也能被删除文件系统都帮我们做好了，不用特殊判断。故主要的工作量就是对文件系统的操作都加以保护即加锁，因此需要在`filesys.h`中定义一个文件锁并且在`filesys.c`中进行初始化；此外，进程需要管理好器打开的文件，因此对于每一个文件可以封装成为一个结构体来管理，该结构体包含了文件的描述符和文件本身，同时在线程中添加列表来方便管理其打开的文件。在基础的属性都定义好之后只需要按照一定的顺序即可完成对文件系统调用的处理：

1. 解析用户栈获得 fd 和其他参数
2. 根据 fd 获得文件指针
3. 根据要求来完成相应的系统调用同时注意上锁
4. 将返回值写入`EAX`中

需要注意的是对于`READ`和`WRITE`来说要考虑到`STDIN`和`STDOUT`以及在调用`CLOSE`的时候需要将文件从打开它的线程中移除并将资源块释放，这部分内容在`thread_exit`中完成

### Synchronization

#### 父进程等待子进程加载完毕后被唤醒

&emsp;主要是针对`EXEC`的任务要求，如果程序由于任何原因无法加载或运行，则必须返回 pid -1，否则不应为有效 pid。 因此，父进程在知道子进程是否成功加载其可执行文件之前不能从 exec 返回。通过在`process_execute`的`thread_create`后使得当前父线程被阻塞，直到在`start_process`中子进程正确加载才唤醒父进程继续执行。

#### 父进程等待子进程结束后被唤醒

&emsp;主要是针对`WAIT`的任务要求，等待子进程`PID`并检索子进程的退出状态，即父进程必须等待其所等待的子进程全部执行完才能够被唤醒继续执行，这也是为了确保 Pintos 在初始进程退出之前不会终止而实现的，因此在`process_wait`中如果检索到符合条件的子进程就通过`sema_down`将自身阻塞，等待该进程执行完毕后调用`thread_exit`才将父进程通过`sema_up`唤醒；

#### 文件系统相关的调用

&emsp;这一部分的同步主要是对同步锁的使用，在`filesys.h`中定义的`filesys_lock`需要在每次进行文件系统相关的调用前后申请锁和释放锁，其中有一个比较容易忽视的地方是`start_process`中`load`函数内部其实也涉及到了文件系统的相关调用，如果不加锁会出现`timeout`的结果。

### Rationale

&emsp;在父子进程中引入`child_entry`和在文件系统调用中引入`file_entry`并通过`struct thread`维护`child_list`和`file_list`我认为能够更加高效地取维护和完成任务所需的代码，通过系统提供的`list`相关的库我们可以较为轻松地将我们想做到的用代码方式来实现；同时通过信号量来控制同步也是相对来说更加高效且简单的方式。此外，在访存检查中使用文档提到的第二种方式也能够让执行效率变得更快。

## Mission 4 Denying Writes to Executables

### Data Structures

新建或修改的 struct 或 struct 成员、全局或静态变量、typedef 或枚举的声明

```c
struct thread{
  ...
  struct file* exec_file;    //由线程创建的可执行文件
}
```

新建或修改的函数

```c
  //一个进程自己正在运行的可执行文件不应该能够被修改于是将自己的可执行文件打开并存入该指针来拒绝写入
static void start_process (void *file_name_)
//关闭当前线程的可执行文件（会自动允许写入）
void process_exit (void)
```

### Algorithms

> 添加代码以拒绝写入用作可执行文件的文件。许多操作系统都这么做，因为如果进程尝试运行当时正在磁盘上进行更改的代码，结果将无法预测。特别是在项目 3 中实现虚拟内存后，这一点很重要，但即使现在，危险也很大。您可以使用`file_deny_write()`防止写入打开的文件。在文件上调用`file_allow_write()`将重新启用它们（除非文件被另一个打开程序拒绝写入）。关闭文件也会重新启用写操作。因此，要拒绝写入进程的可执行文件，只要进程仍在运行，就必须保持打开状态。

&emsp;针对该问题，官方文档提供了一个较好的解决方案，因此只需要在`load`成功后对该线程所打开的可执行文件执行`file_deny_write()`即可，而对于用户程序终止之后文件被关闭（会自动允许写入）

### Synchronization

&emsp;该任务涉及到的同步需求就是任务要求本身，需要避免一个进程自己正在跑的可执行文件被修改，而我们的算法正是在做这一部分的事情，同时需要注意是的涉及文件才做如此处的`filesys_open`前后也需要加锁和释放锁。

### Rationale

&emsp;仅依赖官方文档的提示，并无特别理由。

## Question And Solution For Project2

&emsp;问题 1：在探索 Project2 并完成参数分解后企图在控制台上打印出相关的运行结果时始终只显示`(args)begin`这一行结果，即无法顺利将所有分解得到的参数都输出。  
&emsp;解决方法：初步认为是系统调用的问题，即当系统调用`write`并通过`syscall`转到中断处理程序后被我定义的`syscall_write`所接收时发生意外；经过对系统调用整个过程的深入研究后发现整体逻辑没有任何错误；此时向 CSDN 上的博主`@Altair_Alpha_`请教后发现系统调用进行`write`的时候本身便是一行一行调用的，我之前的误区在于认为系统调用的写是将所有的内容都先写入缓冲区后再输出到控制台上，后来发现是每一行的数据都会调用一次`write`使得所有的内容会分开展示，这样就把问题聚焦到为何只发生了一次系统调用而不是多次上面；审查代码后发现罪魁祸首是`pintos`本身自带的`syscall_handler`中的末尾写了`thread_exit()`使得每次系统调用只发生了一次该进程便被终结；将其删除后即可顺利输出所有的内容！
&emsp;问题 2：在完成访存检查时运行`make check`后发现许多错误都和`page fault`有关
&emsp;解决方法：初步认为是访存检查函数存在漏洞，我采用的是 Pintos 官方文档推荐的第二种写法，即

> 仅检查用户指针是否指向“PHYS_BASE”下方，然后解引用。无效的用户指针将导致“页面错误”，您可以通过修改“userprog/exception.c”中的“page_fault()”的代码来处理。因为它利用了处理器的 MMU,因此倾向于在实际内核（包括 Linux）中使用。

因此在`syscall.c`中定义了`get_user`和`put_user`函数来配合处理已经位于`PHYS_BASE`下方的地址，如果解引用错误就会触发错误转到`exception.c`中的`page_fault`函数中，我发现我将由系统调用导致的页面错误的判断写在输出页面错误之后了，将其移至`print(.......)`前并返回即可。

&emsp;问题 3：在完成实验后`make check`时`tests/userprog/no-vm/multi-oom`测试`fail`
&emsp;解决方案：经过阅读其测试代码发现其本质是一个检查内存泄露的测试，即通过不断创建进程和打开文件来压榨已有的内存空间，创建以及打开后将其释放，如果有未释放的内存，那么就会在内存空间中遗留，此时测试程序重复上述创建进程和打开文件的过程，就会使得内存中剩余的空间越来越小以致于不支持新创建的那一部分进程容纳，于是我做的就是在修改的文件`thread.c`和`process.c`中对`malloc`和`palloc_get_page`进行检查，对于创建的任何一个内存页和内存块都检查其`free`和`palloc_free_page`，要注意的就是不要提前将后续函数中可能要用到的内存提前删除导致缺页中断。

&emsp;问题 4：在完成实验后`make check`时`tests/filesys/base/syn-write`以及`tests/filesys/base/syn-read`测试`fail`
&emsp;解决方案：经过对测试的代码研究后发现其这两个`test`的本质是类似的，因此以对`syn-read`为例，其测试代码首先创建了一个`stuff`的主线程，并通过调用事先写好的`lib`库函数来使用同名文件创建十个子线程，而在这个过程中由于执行速度的差异性问题，会导致每次运行的结果可能都不一样即最终都是因为`load`函数加载文件时冲突导致的（逐层深入发现应该是因为在`dir_look_up`内的`inode_open`内将该文件`block_read`了），因此我尝试在`start_process`中执行`load`函数的前后分别进行加锁和锁的释放，那么就可以有效避免这个问题的发生。

# Project 4：File Systems

## Background

&emsp;在进行文件系统的具体代码编写之前首先需要知道文件系统是如何工作的，`Pintos`本身已经为我们提供了基本的文件创建和删除功能，但是文件本身是固定大小且连续存放的即一个文件占据磁盘上的一个基本扇区。文件在**磁盘**中的组织形式是 inode_disk 即索引节点的磁盘形式，现在 `Pintos`文件是连续的，而且在创建时指定其大小后，再不能改变大小，由起始扇区和文件大小就能找到所有文件数据。目录在磁盘中的组织形式与文件在本质上没有什么不同即都是`inode_disk`，而文件和目录在**内存**中的的组织形式是`inode`即索引节点，最终`inode`封装成为`dir`和`file`，文件比目录多一个参数即是否拒绝写入。对于磁盘来说，其本身的组织形式采用位图来表示，位图文件大小显然与磁盘大小有关，用一个位表示一个扇区是否被分配，若一个扇区 512 字节，一个扇区作为一个物理块，则假设创建一个 2M 的磁盘，有$1024*1024*2/512bit=4096bit= 4096/8=512 Byte$ ，即需要大小为 512bit 的位图来表示，此外需要注意的是位图本身也占据一个扇区，即 0 号扇区，位图的内容存储在磁盘的 0 号扇区中。  
&emsp;在了解文件和目录的组织形式后，首先介绍文件系统的初始化，在`init.c`的`main`函数中调用了`filesys_init`函数来完成文件系统的初始化，在该函数中首先调用`free_map_init`初始化位图文件，包括调用`bitmap_create`来创建位图同时将位图的 0 号扇区标识为位图本身，1 号扇区标识为根目录，注意此时位图还处于内存中，根目录还未在内存中创建，两者都未存储到磁盘中，创建这个文件时显然需要分配磁盘空闲块，就从在内存中的 `free_map `位图中分配就可以，此后`filesys_init`调用`do_format`来真正意义上创建位图和根目录，首先通过`free_map_create`创建位图的`inode`其次通过`dir_create`来创建根目录的`inode`，而`inode`的创建本身又需要调用`inode_create`函数，值得一提的是`inode`在创建的过程中会同时创建`inode_disk`即文件在磁盘中的组织形式，并在本质上将`inode_disk`通过`block_write`相关调用写入到磁盘中进行存储，因此此时磁盘中存储了位图文件和根目录文件。最终`filesys_init`调用`free_map_open`来将写入磁盘的位图文件再次读取到内存中，直到系统关闭才将其写回到磁盘。
![Alt text](pic/%E6%96%87%E4%BB%B6%E7%B3%BB%E7%BB%9F%E5%88%9D%E5%A7%8B%E5%8C%96.png)

<center>文件系统初始化</center>

&emsp;当文件系统初始化之后介绍文件的创建与打开，首先介绍文件的创建，系统通过调用`filesys_create`来创建文件，首先调用`free_map_allocate`来为文件本身分配一个可用的扇区编号，其次通过`inode_create`来根据指定的扇区来创建指定大小的文件，最后调用`dir_add`来通过扇区编号和文件名称来生成一个`dir_entry`即文件入口并将其加入到目录中
&emsp;其次介绍文件的打开，系统通过调用`filesys_open`来打开文件，首先调用`dir_open_root`来打开根目录，并根据打开的根目录从`dir_lookup`中根据传入的文件名称来找到`dir_entry`即文件入口，根据文件入口中的扇区编号将磁盘内容读取到`inode_dick`中并将其作为`data`赋值给`inode`，并根据是文件还是目录将`inode`封装为`file`或`dir`。
![Alt text](pic/%E6%89%93%E5%BC%80%E6%96%87%E4%BB%B6&%E5%88%9B%E5%BB%BA%E6%96%87%E4%BB%B6.png)

<center>打开文件 创建文件</center>

&emsp;接下来按照官方文档推荐的顺序即`Mission 3`->`Mission 1`->`Mission 2`来完成`Project4`

## Mission 1 Indexed and Extensible Files

### Data Structures

新建或修改的 struct 或 struct 成员、全局或静态变量、typedef 或枚举的声明

```c
#define INDIRECT_PTRS 128 // 每个磁盘扇区指定为128个Block块
// 将文件通过索引来组织其Block块的分配，本来是直接分配一个扇区，这将形成许多外部碎片，现在通过分级索引来分页组织
#define DIRECT_BLOCKS 4        // 直接索引
#define INDIRECT_BLOCKS 9      // 一级索引
#define DOUBLE_DIRECT_BLOCKS 1 // 二级索引
#define INODE_PTRS 14          // 每个文件的索引数组总长度为14由4个直接索引+9个一级索引+1个二级索引组成
struct inode_disk
{
  ...
  uint32_t direct_index;          // 直接索引
  uint32_t indirect_index;        // 一级索引
  uint32_t double_indirect_index; // 二级索引
  block_sector_t blocks[14];      // 索引数组
  bool is_dir;                    // 是否是目录
  block_sector_t parent;          // 父文件（目录）的扇区编号
};

struct inode
{
  ...
  off_t length;                   // 文件长度
  off_t read_length;              // 文件的可读取长度
  uint32_t direct_index;          // 直接索引
  uint32_t indirect_index;        // 一级索引
  uint32_t double_indirect_index; // 二级索引
  block_sector_t blocks[14];      // 索引数组
  bool is_dir;                    // 是否是目录
  block_sector_t parent;          // 父文件（目录）的扇区编号
  struct lock lock;               // 文件锁
};
```

新建或修改的函数

```c
// 根据inode_disk的相关信息分配inode空间后将inode和inode_disk的信息同步
bool inode_alloc(struct inode_disk *inode_disk);
// 将指定的inode扩展到length长度
off_t inode_grow(struct inode *inode, off_t length);
// 释放inode所占据的空间
void inode_free(struct inode *inode);
// 获取length长度的inode文件pos位置所处在的物理块扇区编号
static block_sector_t byte_to_sector(const struct inode *inode, off_t length, off_t pos)
// 根据指定扇区号创建一个length长度的文件包括disk_inode和inode同时指定其是否是目录
bool inode_create(block_sector_t sector, off_t length, bool is_dir)
//  通过获取读取扇区内容到inode_disk来获取inode内容并返回
struct inode* inode_open(block_sector_t sector)
//  增加关闭inode的时候将数据写回
void inode_close(struct inode *inode)
// 从inode的offset开始读取size个buffer中的内容，需要考虑到offset+size超出inode的总长度这种情况
off_t inode_read_at(struct inode *inode, void *buffer_, off_t size, off_t offset)
// 从inode的offset位置开始将buffer缓冲区中的size个byte写入扇区，需要考虑offset+size大于inode的总长度
// 在扩容的时候需要加锁
off_t inode_write_at(struct inode *inode, const void *buffer_, off_t size,off_t offset)
// 直接返回inode的长度
off_t inode_length(const struct inode *inode)
//  增加Cache的初始化
void filesys_init(bool format)
// 加入is_dir的判断
bool filesys_create(const char *name, off_t initial_size, bool is_dir)
// 修改inode_create函数调用
void free_map_create(void)
// 修改filesys_create的调用
void fsutil_extract(char **argv UNUSED)
// 修改filesys_create的调用
static void syscall_create(struct intr_frame *f)
// 修改inode_create的调用
bool dir_create(block_sector_t sector, size_t entry_cnt)
// 判断inode是否是目录
bool inode_is_dir(const struct inode *);
// 获取inode的open_cnt值
int inode_get_open_cnt(const struct inode *);
// 获取inode的父目录所在扇区编号
block_sector_t inode_get_parent(const struct inode *);
// 将指定的parent扇区作为child扇区的父文件/目录
bool inode_set_parent(block_sector_t parent, block_sector_t child);
// 为inode加锁
void inode_lock(const struct inode *inode);
// 为inode解锁
void inode_unlock(const struct inode *inode);
// 将缓冲区的内容写回到磁盘中
void filesys_done(void)
// 给inode操作加锁
bool dir_readdir(struct dir *dir, char name[NAME_MAX + 1])
// 目录非使用和非空判断，对inode操作加锁
bool dir_remove(struct dir *dir, const char *name)
// 为inode的操作加锁
bool dir_add(struct dir *dir, const char *name, block_sector_t inode_sector)
// 进行inode的操作前后加锁
bool dir_lookup(const struct dir *dir, const char *name, struct inode **inode)
```

### Algorithms

&emsp;当前 `Pintos` 文件系统限制很大。文件需要连续存储，这会导致大量磁盘碎片。文件大小固定，不能动态增长文件，只有一个目录。本专题主要把连续存储方式改为了混合索引结构，而且可以动态增长文件，可以创建子目录。基本文件系统将文件分配为单个扩展区，从而使其容易受到外部碎片的影响，也就是说，即使 n 块是 free 的，也可能无法分配 n 块文件。 通过修改磁盘上的 inode 结构消除此问题。 实际上，这可能意味着使用具有直接，间接和双重间接块的索引结构，本专题主要是参考了`Linux`文件系统的实现即将文件的内容通过三级索引来组织，因此需要在`inode`和`inode_disk`结构体中引入直接索引、一级索引和二级索引属性。在创建文件的过程中首先创建文件在磁盘中的组织形式`inode_disk`其后根据该`inode_disk`来生成`inode`同时将数据回写给`inode_disk`，创建文件的过程核心是`inode_grow`即文件的扩展函数，在该函数中根据文件的大小来生成对应的直接索引、一级索引和二级索引，该函数保证了只要有可用空间，索引节点就可以使文件增长成为可能可用，实现文件增长。 在基本文件系统中，创建文件时指定大小，然而在最现代的文件中系统，最初创建的文件大小为 0，然后展开每次从文件末尾进行写操作，同时可以使得文件大小没有限制以及根目录扩展到超出其初始限制的 16 个文件，同时能够允许用户查找超出当前文件结尾的范围。
![Alt text](pic/%E4%B8%89%E7%BA%A7%E7%B4%A2%E5%BC%95.png)

<center>三级混合索引组织架构</center>

&emsp;在三级索引的基础上对文件进行读写时，都需要考虑是否会超出该文件的最大范围，对于读操作，如果在指定偏移量开始所需要读取的大小大于该文件的最大长度那么就需要将其限制到最大长度，避免读取到超过文件最大长度的内容，而对于写操作则需要考虑是否扩容，如果偏移量加写文件的大小超过文件结尾那么首先就需要将文件进行扩容。
![Alt text](pic/%E8%AF%BB%E5%86%99%E6%89%A9%E5%B1%95.png)

<center>读写扩展</center>

### Synchronization

&emsp;本专题需要考虑的同步问题主要是`inode_grow`过程的前后需要加锁和释放锁，避免因为多个异步请求同时扩展文件而导致数据不一致，同时对于`inode`的许多操作包括打开`inode`、为`inode`设置父文件（本质也需要打开`inode`）、`inode`的读写操作、`inode`的移除操作等都需要加锁和释放锁。

### Rationale

&emsp;通过`Linux`的三级索引来组织文件系统，这样的文件其实就是所谓的`稀疏文件`，同时在不超过直接索引占据的物理块的小文件只需要两次寻址首先读`inode`其次读数据块，对于大文件而言最多也只需要四次寻址即读取`inode`、读取一级索引、读取二级索引、读取数据块。这样的方式可以很轻松地将大文件进行组织且生成的速度非常之快，同时可扩展性非常强。

## Mission 2 Subdirectories

### Data Structures

新建或修改的 struct 或 struct 成员、全局或静态变量、typedef 或枚举的声明

```c
struct thread
{
    ...
  struct dir *dir; // 该线程所处的目录位置
};
#define FD_MAX 128             // 可创建的文件最大数量
```

新建或修改的函数

```c
// 判断dir是否是根目录
bool dir_is_root(struct dir *dir)
// 获取dir的父目录
struct inode *dir_parent_inode(struct dir *dir)
// 判断目录是否为空，即判断是否有被占据的dir_entry
bool dir_is_empty(struct inode *inode)
// 添加父子文件的设置
bool dir_add(struct dir *dir, const char *name, block_sector_t inode_sector)
// 将当前正在运行的线程的目录更改为指定目录
bool filesys_chdir(const char *name);
// 根据path_name获取到该路径结尾的文件/目录名称
char *path_to_name(const char *path_name);
// 根据path_name获取最底层的目录
struct dir *path_to_dir(const char *path);
// 修改为父子目录条件下的文件的创建
bool filesys_create(const char *name, off_t initial_size, bool is_dir)
// 修改为父子目录条件下指定名称文件的打开
struct file *filesys_open(const char *name)
// 修改为父子目录条件下文件的删除
bool filesys_remove(const char *name)
// 将当前正在运行的线程的目录更改为指定目录
bool filesys_chdir(const char *path)
// 为当前线程设置目录
tid_t thread_create(const char *name, int priority, thread_func *function, void *aux)
// 初始化线程的当前目录参数
static void init_thread(struct thread *t, const char *name, int priority)
 // 如果当前线程没有设置目录那么就将根目录设置为其目录
static void start_process(void *file_name_)
// 根据指定的path来修改当前线程的目录
bool syscall_chdir(struct intr_frame *f);
// 根据指定的path来创建目录
bool syscall_mkdir(struct intr_frame *f);
// 将指定dir目录下的下一个已使用的dir_entry的名称赋值给path
bool syscall_readdir(struct intr_frame *f);
// 判断某个文件是否是目录
bool syscall_isdir(struct intr_frame *f);
// 获取某个文件所占据的扇区本身
int syscall_inumber(struct intr_frame *f);
//将新增的系统调用添加到switch case中
static void syscall_handler (struct intr_frame *f)
//在遍历文件的同时将文件根据其是文件/目录进行关闭，同时关闭当前线程的工作目录
void thread_exit(void)
//根据所关闭的文件类型来关闭文件
static void syscall_close(struct intr_frame *f)
// 增加判断读取的文件不是目录
static void syscall_read(struct intr_frame *f)
// 增加判断写入的文件不是目录
static void syscall_write(struct intr_frame *f)
// 增加判断文件不是目录
static void syscall_seek(struct intr_frame *f)
// 增加判断文件不是目录
static void syscall_tell(struct intr_frame *f)
// 增加判断文件是否是目录
static void syscall_filesize(struct intr_frame *f)
```

### Algorithms

&emsp;本专题需要实现一个分层的名称空间。在基本文件系统中，所有文件位于单个目录中。 我们将修改它以允许目录指向文件或其他目录的条目。确保目录可以像其他任何文件一样扩展超出其原始大小。为每个进程维护一个单独的当前目录。 在启动时，将根目录设置为初始进程的当前目录。 当一个进程通过 `exec` 系统调用启动另一个进程时，子进程将继承其父进程的当前目录，但是之后，两个进程的当前目录是独立的，因此更改其自己的当前目录不会对另一个进程产生影响。更新现有的系统调用，以便在调用方提供文件名的任何地方，都可以使用绝对或相对路径名。目录分隔符为正斜杠（“/”），还必须支持特殊文件名“.”和“..”，由于之前的文件系统中目录文件只有根目录，因此不需要对文件进行判断是否是目录，除了根目录外的都是文件，而本专题为了能够实现子目录的效果，需要给每一个`inode`添加是否是目录的属性，在创建`inode`节点的时候，首先需要根据传入的文件路径来获取到最底层的目录，并获取到文件名，在此基础上即可将新生成的`inode`添加到该最底层的目录中；同时为了适应相对路径和绝对路径，因此在`filesys_open`的过程中需要判断文件名是`.`或者`..`来获取当前目录或者父目录，如果是普通名称那么就在目录中查找该文件，最后根据打开的是否是目录来调用对应的函数，同时为了能够让子进程继承其父进程的当前目录那么需要在`thread_create`中添加父子目录的赋值，为当前线程添加目录。最后由于文件系统已经具备了父子目录的能力，因此需要将系统调用进行完善和补充，如根据指定的`path`来修改当前线程的目录、根据指定的`path`来创建目录、判断某个文件是否是目录、获取目录中下一个已使用的`dir_entry`的名称、获取某个文件所占据的扇区编号。因此由于目前`inode`文件和目录已经有严格的界限因此在关闭文件、读取文件、写文件、修改位置等等操作时都必须得确保该文件不是目录。

### Synchronization

&emsp;目录其本质也是文件因此其同步方式也只需要考虑`inode`相关的同步。

### Rationale

&emsp;本专题中的实现方式考虑了绝对路径和相对路径的使用同时支持特殊文件名即`.`和`..`，具有较高的通用性和可扩展性。

## Mission 3 Buffer Cache

### Data Structures

新建或修改的 struct 或 struct 成员、全局或静态变量、typedef 或枚举的声明

```c
在Makefile.build中加入filesys_SRC += filesys/cache.c
#define CACHE_MAX_SIZE 64 // 最大Cache数组大小
// Cache块
struct disk_cache
{

    uint8_t block[BLOCK_SECTOR_SIZE]; // Cache块大小为磁盘中的块区大小即BLOCK_SECTOR_SIZE大小
    block_sector_t disk_sector;       // Cache块在磁盘中扇区编号

    bool is_free;  // 该Cache块是否空闲
    int open_cnt;  // 该Cache块被访问/打开的次数
    bool accessed; // Cache块是否被访问
    bool dirty;    // 该Cache块是否被写入
};

struct lock cache_lock;            // Cache的同步锁
struct disk_cache cache_array[64]; // Cache块组成的数组
```

新建或修改的函数

```c
// 初始化Cache数组中的每一块
void init_entry(int idx);
// 初始化Cache数组同时初始化Cache的同步锁
void init_cache(void);
// 根据磁盘中的扇区号获取Cache数组中对应块的索引
int get_cache_entry(block_sector_t disk_sector);
// 获取Cache数组中当前空闲的块
int get_free_entry(void);
// 访问指定扇区的Cache块并修改其对应的属性，如果不存在该扇区的Cache块那么就替换一个Cache块再修改
int access_cache_entry(block_sector_t disk_sector, bool dirty);
// Cache块的替换算法，基于访问位和修改位的时钟算法，性能最接近LRU
int replace_cache_entry(block_sector_t disk_sector, bool dirty);
// 每隔四个TIMER_FREQ将缓冲区中的数据写回磁盘
void func_periodic_writer(void *aux);
// 将Cache数组中所有dirty为true即被修改过的Cache块写回磁盘并更新dirty为false，根据是否clear来确定是否将该缓存快初始化
void write_back(bool clear);
// 预取策略，如果Cache数组中没有指定扇区块那么替换一个Cache块
void func_read_ahead(void *aux);
// 预取当前块的下一块到缓存区中
void ahead_reader(block_sector_t);
```

### Algorithms

&emsp;在创建缓存块的基本结构时，其大小必须与磁盘扇区的大小相同即 512 字节，同时需要属性来标识其所对应的磁盘扇区编号、打开数量和是否空闲，其中是否空闲属性是完成官方需求中“根据需要将空闲映射表的缓存副本永久保留在内存中”所必须的，而打开数量则是替换算法所必须的，我们必须保证当前正在被访问的缓存块避免被替换，此外还有两个属性即是否被访问和是否被修改，这两个属性构成了替换策略的核心，即当缓冲区已满的时候，将从指针当前位置开始，扫描缓冲区，在这次扫描中对使用位不做任何修改，选择遇到的第一个访问位和修改位都为 0 的缓冲块进行替换，若第一步失败则重新扫描，查找访问位为 0 同时修改位为 1 的缓冲块，选择遇到的第一个用于替换，在这次扫描中将每个跳过的缓冲块的使用位置为 0，若第二步失败则指针回到最开始的位置，且集合中所有缓冲块的使用位已经均为 0，重复第一步并在必要时重复第二步即可找到供置换的缓冲块。在缓冲区设计中替换算法便是其核心算法。在文件系统初始化的时候会调用缓冲区的初始化函数，在该函数中将所有缓冲块的属性初始化，同时开启一个子线程来完成“后写式”的需求，即每隔固定的时间来将缓冲区中所有缓冲块的内容写回到磁盘中，在写回的时候指定此时并不将缓冲区从内存中移除而是继续保留在内存中，这又完成了文档中的另一个需求。此时，已经具备了基本的缓冲区功能，只需将缓冲区效果整合到文件的读取中即可，在读取和写入的时候首先通过扇区号获取缓冲区中的缓冲块，在读取时将缓冲块内从指定偏移量开始的指定大小的内容读取到指定的`buffer`中，同时将该缓冲块的访问位置为 1 读取完毕后将其打开数量减少，写入操作时与读取异曲同工不加以赘述。

### Synchronization

&emsp;不同缓存块上的操作必须是独立的，且确保对同一缓冲块的内容进行修改的同时不被打断，因此需要引入缓冲块的同步锁，在缓冲区初始化的过程中初始化同步锁，同时当线程访问缓冲块、回写以及进行`read_ahead`的过程前后需要加锁和释放锁避免数据在异步条件下不一致。

### Rationale

&emsp;文档上要求缓存替换算法不比时钟算法差，那么首先我默认此处的时钟算法指的是最基本的时钟算法即只考虑访问位的情况，那么我在此基础上增加了修改位来优化时钟算法，页面置换算法在缓冲区的所有页中循环，查找至今未被修改且最近未被访问的页，这样的页最适合被置换，并且由于增加了修改位，在回写的过程中可以只将被修改的位写回磁盘而不需要为所有的缓存块执行该操作。这种算法优于简单时钟算法，置换时首选无变化的页同时优于修改过的页在置换前需写回这样可以节省时间。时钟算法的性能非常接近`LRU`即最近最少使用算法。

# 答辩过程中老师问的问题

## Alarm Clock 的具体实现细节

`time_sleep` 原函数，其实现逻辑为获取机器开机以来经历的 `ticks` 数并记录为`start`作为忙等待的起始时间，此后确保不存在外部中断的前提下调用`time_elapsed`即获取当前时间和`start`的时间差并与期望等待时间`ticks`进行比较，如果尚未达到，那么就调用`thread_yield`，分析可以发现该函数的作用是让出 CPU 的控制权并将当前线程放入就绪队列中，并重新进行调度，而循环则可以使得该过程不断进行，造成的结果是 CPU 不断进行调度即`BUSY WAIT`，而我为了解决忙等待的问题，采用的解决方法就是定义了一个结构体即`sleep_thread_entry`来存储当前线程和当前线程的睡眠时间，当调用`time_sleep`函数的时候就将当前线程封装为`sleep_thread_entry`阻塞当前线程后加入到睡眠线程队列中，对于所有处于该睡眠线程队列中的线程，在每个`tick`都会执行的时间中断函数中对线程的睡眠时间进行递减，当递减到零时，就将对应的线程从睡眠线程队列中移除同时解除阻塞，为了与优先级调度相关的要求相对应，解除阻塞后还需要在关中断的环境下进行一次线程让步，使得被唤醒且具有高优先级的线程能够重新得到 CPU 资源。

## 访存检查的页面错误机制

分析`Pintos`的内存管理机制，我们可以发现其内存管理依赖于**页式虚拟内存**，其中内核内存是全局性的而用户程序则通过页表完成对应的映射，因此我采用的访存检查方式是官方文档中所推荐的第二种，即仅检查用户指针是否指向`PHYS_BASE`下方，然后解引用。可以理解为仅做前者的检查然后就访问，如果不合法会引发`page fault`，然后再处理这个异常。基于这种方式的访存检查利用了处理器的 **MMU** 因此速度会更快。在访存检查的过程中，如果地址不合法那么就会跳转到`exception.c`文件的`page_fault`函数中完成异常处理，前面提到内核内存是全局性的因此一般不会因为内核代码本身的调用而发生页错误，如果在内核态发生了页错误那么就可以认为是由系统调用引起的，而是否是内核态的页面错误可以简单通过`page_fault`中本身定义的`user`变量来判断，如果`user`变量为`false`就代表是内核态错误。

## 文件系统索引的组织形式

文件系统的组织方式主要采用的是**混合索引**，即将文件的内容通过**索引数组**来表征，在`inode`结构体中定义大小为 14 的`blocks`数组来表征文件的具体内容，对于这十四个索引而言，其中四个索引采用的是直接索引块的方式、九个索引采用的是一级间接索引块的方式、最后一个索引采用的是二级间接索引块的方式来组织的。虽然看似是 4：9：1 的形式，但是这里是 4：9：1 并不是索引块本身的范围，即并不意味着直接索引范围是 1~4， 而是索引数组本身大小的 1~14 都是直接索引，而在 1~4 范围内是直接映射、在 5~13 范围内是通过一级索引间接映射，在最后一块是通过二级索引间接映射。这三个参数在`inode`结构体中分别标识为`direct_index`、`indirect_index`、`double_indirect_index`，假设某个`inode`的三个属性值分别为 14，128，256 那么就意味着其最后一个物理块直接索引是 14 即索引数组的最后一个，在该数组所映射的物理块中整个物理块作为索引数组找到第 128 块作为一级间接索引，同时将该索引对应的物理块再次作为索引数组找到第 256 个物理块作为最后一个物理块的内容。

# 专业认证

## 对专业知识基本概念、基本理论和典型方法的理解。

忙等待的解决深化了阻塞态的理解，优先级调度则引入了捐献的概念，高级调度提供了不一样的优先级计算方法，从不同角度对线程部分的基本理论进行的辐射；同时参数分离迫使我们不得不研究操作系统中用户栈的概念，用户空间是从高地址向低地址填充的，系统调用则令我们理解了操作系统是通过中断的方式来处理系统调用的，同时理解了访存检查的重要性；文件系统则引入了缓冲区和混合索引的基本理论，在缓冲区的替换算法中则用到了“时钟算法”这个典型方法。

## 怎么建立模型

客观世界几乎一切的规律原则一般来说都可以在数学中找到它们的表现形式，将客观条件模型化，同时将客体的属性以“运动”变化规律数学公式化，这就建立了数学模型。这里最为典型的例子便是 Pintos Project 1 中的 Mission 3 通过归纳得到 Priority 的计算公式，并对每个参数都给以具体的含义，如 recent_cpu 代表其最近所占据的 CPU 时间的量化表达，是一种变化的规律，而系统平均负载 load_avg 也是同理归纳，最终通过一个公式来计算得到线程某一时刻的优先级。

## 如何利用基本原理解决复杂问题

复杂工程问题是工科专业的核心能力目标，但过于笼统、抽象，难以在课程中真正落地。因此通过本次 Pintos 的课程设计，我们可以将该课设当做一个复杂的工程问题。对于复杂的工程问题我们一般来说首先需要具备一定的工程知识，如对 Pintos 而言是需要掌握操作系统的基本原理包括（线程、同步和互斥、内存和文件系统），其后根据基本原理来针对问题进行基本的原理映射即该问题对应的是哪部分的基本原理，最后通过自主学习、沟通合作、问题分析来提出基本的解决方案，并在整个过程中培养工程观念和社会责任。

## 具有实验方案设计的能力

实验方案设计能力是一个相对而言较为抽象的词语，但是在本次 Pintos 的实验中我们可以将其具象化到每一个 Project 较为困难的 Mission 中，比如 Project 1 的 Mission 2 优先级调度、Project2 的 Mission 3 系统调用、Project 4 的 Mission 2 索引和可扩展文件中，在以上的每个 Mission 中我们都需要对任务的内容进行分点归纳，并将任务本身和操作系统的理论知识进行关联，如优先级调度的锁、捐献，索引和可扩展文件用到的文件系统三级索引（混合索引）等等，此后在此基础上设计对应的代码部分并实践的同时不断查错最后完成该部分，总而言之包括了任务内容抽取、理论知识关联、实践设计、查错修改、完成实验这几个基本步骤。

## 如何对环境和社会的可持续发展

操作系统作为支撑计算机有序运行的底层基础技术，已经成为信息化时代一台计算机的灵魂所在，而灵魂往往是最难以被触及的。通过本次对 Pintos 运行原理的理解和各任务的实现，令我对操作系统的灵魂有了更深入的认识，Pintos 可谓是“麻雀虽小，五脏俱全”，然而我们不得不承认其本身只是一个轻量级的大多情况下仅作为教学用途的课程资料，因此我们可以想象更加完善的操作系统如 Unix、Linux 和 Windows 等等操作系统在开发和设计过程中所面临的的巨大挑战，因此我们不能因为操作系统无法被用户可视化感知全貌而忽略操作系统的设计难度以及操作系统在各种设备中的重要地位和作用，设计更加高效的操作系统进行线程管理、内存管理、文件系统管理等等能够很大程度上提高有限资源的利用效率，因此我们可以利用有限的资源做更多的任务从而促进社会的可持续发展，此外随着操作系统公司对新的通用终端产品的接纳更多智能化设备将得以升级，如智能扫地机器人、智能垃圾分配机器人都得以在智能终端的支持下得以升级从而对环境有着良好的改善效果，同时传统的经典控制模型正在向新型控制模型转变，操作系统也将逐渐适应某些具体领域的需求而专业化，从通用转向特化，如车载操作系统向智能座舱操作系统演变已经成为必然的趋势，那么将会为人类社会提供更加便捷化的生活体验。
