#include "userprog/process.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "userprog/gdt.h"
#include "userprog/pagedir.h"
#include "userprog/tss.h"
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "threads/vaddr.h"

static thread_func start_process NO_RETURN;
static bool load(const char *cmdline, void (**eip)(void), void **esp);

/* Starts a new thread running a user program loaded from
   FILENAME.  The new thread may be scheduled (and may even exit)
   before process_execute() returns.  Returns the new process's
   thread id, or TID_ERROR if the thread cannot be created. */
tid_t process_execute(const char *file_name)
{
  char *fn_for_process_name, *fn_for_start_process_arguments;
  tid_t tid;

  // 为FILE_NAME生成两份拷贝，其中一份用于线程名，另一份用于start_process的参数
  // 目的主要是为了避免发生访存冲突
  fn_for_process_name = palloc_get_page(0);
  fn_for_start_process_arguments = palloc_get_page(0);
  if (fn_for_process_name == NULL || fn_for_start_process_arguments == NULL)
    return TID_ERROR;
  strlcpy(fn_for_process_name, file_name, PGSIZE);
  strlcpy(fn_for_start_process_arguments, file_name, PGSIZE);

  // 通过Pintos文档提示的strtok_r函数来分割字符串
  char *save_ptr;
  char *process_name = strtok_r(fn_for_process_name, " ", &save_ptr);
  // 将分割后的字符串用于线程名称的赋值，将另一份拷贝用于start_process进行处理
  tid = thread_create(process_name, PRI_DEFAULT, start_process, fn_for_start_process_arguments);
  if (tid == TID_ERROR)
  {
    palloc_free_page(fn_for_process_name);
    palloc_free_page(fn_for_start_process_arguments);
  }
  // 将当前线程阻塞
  sema_down(&thread_current()->exec_sema);
  // 如果子进程运行失败了那么就返回-1
  if (!thread_current()->exec_success)
  {
    return -1;
  }
  // 重置执行参数以便下一次调用
  thread_current()->exec_success = false;
  palloc_free_page(fn_for_process_name);
  return tid;
}

/* A thread function that loads a user process and starts it
   running. */
static void
start_process(void *file_name_)
{
  char *fn_for_process_name = file_name_, *fn_for_start_process_arguments;
  fn_for_start_process_arguments = palloc_get_page(0);
  strlcpy(fn_for_start_process_arguments, fn_for_process_name, PGSIZE);
  struct intr_frame if_;
  bool success;

  char *token, *save_ptr;
  char *process_name = strtok_r(fn_for_process_name, " ", &save_ptr);

  /* Initialize interrupt frame and load executable. */
  memset(&if_, 0, sizeof if_);
  if_.gs = if_.fs = if_.es = if_.ds = if_.ss = SEL_UDSEG;
  if_.cs = SEL_UCSEG;
  if_.eflags = FLAG_IF | FLAG_MBS;

  lock_acquire(&filesys_lock);
  success = load(process_name, &if_.eip, &if_.esp);
  lock_release(&filesys_lock);

  if (!success)
  {
    // 如果执行失败就将子进程的状态设置为结束状态同时设置状态码为-1并唤醒父进程
    thread_current()->as_child->is_alive = false;
    thread_current()->exit_code = -1;
    sema_up(&thread_current()->parent->exec_sema);
    palloc_free_page(fn_for_process_name);
    palloc_free_page(fn_for_start_process_arguments);
    thread_exit();
  } // 如果执行成功就将父进程的执行成功标识符置为true并唤醒父进程
  thread_current()->parent->exec_success = 1;
  sema_up(&thread_current()->parent->exec_sema);
  int argc = 0;
  void *argv[128];
  // 第一步：将栈指针指向用户虚拟地址空间的开头
  if_.esp = PHYS_BASE;
  // 第二步：解析参数，将它们放置在栈顶并记录他们的地址，此处单词的顺序无关紧要
  for (token = strtok_r(fn_for_start_process_arguments, " ", &save_ptr); token != NULL; token = strtok_r(NULL, " ", &save_ptr))
  {
    size_t arg_len = strlen(token) + 1; // strlen不算\0因此需要+1
    if_.esp -= arg_len;
    memcpy(if_.esp, token, arg_len);
    argv[argc++] = if_.esp;
  }
  // 第三步：首先将指针向下舍入到4的倍数，然后推送每个字符串的地址
  uintptr_t temp = (uintptr_t)if_.esp; // 指针不能直接进行取模运行
  if (temp % 4 != 0)
  {
    temp -= temp % 4;
  }
  if_.esp = (void *)temp; // 这一步注意不要漏了
  // 第四步：加上堆栈上的空指针哨兵，按从右到左的顺序。
  size_t ptr_size = sizeof(void *);
  if_.esp -= ptr_size;
  memset(if_.esp, 0, ptr_size);
  for (int i = argc - 1; i >= 0; i--)
  {
    if_.esp -= ptr_size;
    memcpy(if_.esp, &argv[i], ptr_size);
    // printf("%s\n",argv[i]);
  }
  // 第五步：将argv的地址即argv[0]压入栈中，使得程序能够在后续访问到上述参数
  if_.esp -= ptr_size;
  *(uintptr_t *)if_.esp = ((uintptr_t)if_.esp + ptr_size);
  // 将argc压入栈中，使得程序能够在后续访问到参数的个数
  if_.esp -= ptr_size;
  *(int *)if_.esp = argc;
  // 第六步：压入一个返回地址
  if_.esp -= ptr_size;
  memset(if_.esp, 0, ptr_size);
  // 文档提示我们使用`hex_dump()`函数来打印内存状况以检验实现的正确性
  // printf("STACK SET. ESP: %p\n", if_.esp);
  // hex_dump((uintptr_t)if_.esp, if_.esp, 100, true); // 打印的byte数不用特别准确，随便填大一些

  // 一个进程自己正在运行的可执行文件不应该能够被修改于是将自己的可执行文件打开并存入该指针来拒绝写入
  lock_acquire(&filesys_lock);
  struct file *f = filesys_open(process_name);
  file_deny_write(f);
  lock_release(&filesys_lock);
  thread_current()->exec_file = f;

  palloc_free_page(fn_for_process_name);
  palloc_free_page(fn_for_start_process_arguments);
  // 如果当前线程没有设置目录那么就将根目录设置为其目录
  if (!thread_current()->dir)
    thread_current()->dir = dir_open_root();
  /* Start the user process by simulating a return from an
     interrupt, implemented by intr_exit (in
     threads/intr-stubs.S).  Because intr_exit takes all of its
     arguments on the stack in the form of a `struct intr_frame',
     we just point the stack pointer (%esp) to our stack frame
     and jump to it. */
  asm volatile("movl %0, %%esp; jmp intr_exit"
               :
               : "g"(&if_)
               : "memory");
  NOT_REACHED();
}

/* Waits for thread TID to die and returns its exit status.  If
   it was terminated by the kernel (i.e. killed due to an
   exception), returns -1.  If TID is invalid or if it was not a
   child of the calling process, or if process_wait() has already
   been successfully called for the given TID, returns -1
   immediately, without waiting.

   This function will be implemented in problem 2-2.  For now, it
   does nothing. */
int process_wait(tid_t child_tid)
{
  struct thread *current_thread = thread_current(); //
  struct list_elem *elem_;
  // 遍历当前进程的所有子进程
  for (elem_ = list_begin(&current_thread->child_list); elem_ != list_end(&current_thread->child_list); elem_ = list_next(elem_))
  {
    struct child_entry *entry = list_entry(elem_, struct child_entry, elem);
    // 如果存在进程所需要等待的子进程tid
    if (entry->tid == child_tid)
    {
      // 如果该子进程并没有被父进程所等待同时子进程并没有结束那么就使用信号量卡住自身来等待子进程运行结束
      if (!entry->is_waiting_on && entry->is_alive)
      {
        entry->is_waiting_on = true;
        sema_down(&entry->wait_sema);
        return entry->exit_code;
      }
      else if (entry->is_waiting_on)
      { // 如果已经在等待该子进程了那么就返回-1
        return -1;
      }
      else
      { // 如果子进程已经结束那么就返回exit_code
        return entry->exit_code;
      }
    }
  }
  return -1;
}

/* Free the current process's resources. */
void process_exit(void)
{

  struct thread *cur = thread_current();
  uint32_t *pd;
  printf("%s: exit(%d)\n", cur->name, cur->exit_code);
  // 关闭当前线程的可执行文件（会自动允许写入）
  lock_acquire(&filesys_lock);
  file_close(cur->exec_file);
  lock_release(&filesys_lock);

  /* Destroy the current process's page directory and switch back
     to the kernel-only page directory. */
  pd = cur->pagedir;
  if (pd != NULL)
  {
    /* Correct ordering here is crucial.  We must set
       cur->pagedir to NULL before switching page directories,
       so that a timer interrupt can't switch back to the
       process page directory.  We must activate the base page
       directory before destroying the process's page
       directory, or our active page directory will be one
       that's been freed (and cleared). */
    cur->pagedir = NULL;
    pagedir_activate(NULL);
    pagedir_destroy(pd);
  }
}

/* Sets up the CPU for running user code in the current
   thread.
   This function is called on every context switch. */
void process_activate(void)
{
  struct thread *t = thread_current();

  /* Activate thread's page tables. */
  pagedir_activate(t->pagedir);

  /* Set thread's kernel stack for use in processing
     interrupts. */
  tss_update();
}

/* We load ELF binaries.  The following definitions are taken
   from the ELF specification, [ELF1], more-or-less verbatim.  */

/* ELF types.  See [ELF1] 1-2. */
typedef uint32_t Elf32_Word, Elf32_Addr, Elf32_Off;
typedef uint16_t Elf32_Half;

/* For use with ELF types in printf(). */
#define PE32Wx PRIx32 /* Print Elf32_Word in hexadecimal. */
#define PE32Ax PRIx32 /* Print Elf32_Addr in hexadecimal. */
#define PE32Ox PRIx32 /* Print Elf32_Off in hexadecimal. */
#define PE32Hx PRIx16 /* Print Elf32_Half in hexadecimal. */

/* Executable header.  See [ELF1] 1-4 to 1-8.
   This appears at the very beginning of an ELF binary. */
struct Elf32_Ehdr
{
  unsigned char e_ident[16];
  Elf32_Half e_type;
  Elf32_Half e_machine;
  Elf32_Word e_version;
  Elf32_Addr e_entry;
  Elf32_Off e_phoff;
  Elf32_Off e_shoff;
  Elf32_Word e_flags;
  Elf32_Half e_ehsize;
  Elf32_Half e_phentsize;
  Elf32_Half e_phnum;
  Elf32_Half e_shentsize;
  Elf32_Half e_shnum;
  Elf32_Half e_shstrndx;
};

/* Program header.  See [ELF1] 2-2 to 2-4.
   There are e_phnum of these, starting at file offset e_phoff
   (see [ELF1] 1-6). */
struct Elf32_Phdr
{
  Elf32_Word p_type;
  Elf32_Off p_offset;
  Elf32_Addr p_vaddr;
  Elf32_Addr p_paddr;
  Elf32_Word p_filesz;
  Elf32_Word p_memsz;
  Elf32_Word p_flags;
  Elf32_Word p_align;
};

/* Values for p_type.  See [ELF1] 2-3. */
#define PT_NULL 0           /* Ignore. */
#define PT_LOAD 1           /* Loadable segment. */
#define PT_DYNAMIC 2        /* Dynamic linking info. */
#define PT_INTERP 3         /* Name of dynamic loader. */
#define PT_NOTE 4           /* Auxiliary info. */
#define PT_SHLIB 5          /* Reserved. */
#define PT_PHDR 6           /* Program header table. */
#define PT_STACK 0x6474e551 /* Stack segment. */

/* Flags for p_flags.  See [ELF3] 2-3 and 2-4. */
#define PF_X 1 /* Executable. */
#define PF_W 2 /* Writable. */
#define PF_R 4 /* Readable. */

static bool setup_stack(void **esp);
static bool validate_segment(const struct Elf32_Phdr *, struct file *);
static bool load_segment(struct file *file, off_t ofs, uint8_t *upage,
                         uint32_t read_bytes, uint32_t zero_bytes,
                         bool writable);

/* Loads an ELF executable from FILE_NAME into the current thread.
   Stores the executable's entry point into *EIP
   and its initial stack pointer into *ESP.
   Returns true if successful, false otherwise. */
bool load(const char *file_name, void (**eip)(void), void **esp)
{
  struct thread *t = thread_current();
  struct Elf32_Ehdr ehdr;
  struct file *file = NULL;
  off_t file_ofs;
  bool success = false;
  int i;

  /* Allocate and activate page directory. */
  t->pagedir = pagedir_create();
  if (t->pagedir == NULL)
    goto done;
  process_activate();
  /* Open executable file. */
  file = filesys_open(file_name);
  if (file == NULL)
  {
    printf("load: %s: open failed\n", file_name);
    goto done;
  }

  /* Read and verify executable header. */
  if (file_read(file, &ehdr, sizeof ehdr) != sizeof ehdr || memcmp(ehdr.e_ident, "\177ELF\1\1\1", 7) || ehdr.e_type != 2 || ehdr.e_machine != 3 || ehdr.e_version != 1 || ehdr.e_phentsize != sizeof(struct Elf32_Phdr) || ehdr.e_phnum > 1024)
  {
    printf("load: %s: error loading executable\n", file_name);
    goto done;
  }

  /* Read program headers. */
  file_ofs = ehdr.e_phoff;
  for (i = 0; i < ehdr.e_phnum; i++)
  {
    struct Elf32_Phdr phdr;

    if (file_ofs < 0 || file_ofs > file_length(file))
      goto done;
    file_seek(file, file_ofs);

    if (file_read(file, &phdr, sizeof phdr) != sizeof phdr)
      goto done;
    file_ofs += sizeof phdr;
    switch (phdr.p_type)
    {
    case PT_NULL:
    case PT_NOTE:
    case PT_PHDR:
    case PT_STACK:
    default:
      /* Ignore this segment. */
      break;
    case PT_DYNAMIC:
    case PT_INTERP:
    case PT_SHLIB:
      goto done;
    case PT_LOAD:
      if (validate_segment(&phdr, file))
      {
        bool writable = (phdr.p_flags & PF_W) != 0;
        uint32_t file_page = phdr.p_offset & ~PGMASK;
        uint32_t mem_page = phdr.p_vaddr & ~PGMASK;
        uint32_t page_offset = phdr.p_vaddr & PGMASK;
        uint32_t read_bytes, zero_bytes;
        if (phdr.p_filesz > 0)
        {
          /* Normal segment.
             Read initial part from disk and zero the rest. */
          read_bytes = page_offset + phdr.p_filesz;
          zero_bytes = (ROUND_UP(page_offset + phdr.p_memsz, PGSIZE) - read_bytes);
        }
        else
        {
          /* Entirely zero.
             Don't read anything from disk. */
          read_bytes = 0;
          zero_bytes = ROUND_UP(page_offset + phdr.p_memsz, PGSIZE);
        }
        if (!load_segment(file, file_page, (void *)mem_page,
                          read_bytes, zero_bytes, writable))
          goto done;
      }
      else
        goto done;
      break;
    }
  }

  /* Set up stack. */
  if (!setup_stack(esp))
    goto done;

  /* Start address. */
  *eip = (void (*)(void))ehdr.e_entry;

  success = true;

done:
  /* We arrive here whether the load is successful or not. */
  file_close(file);
  return success;
}

/* load() helpers. */

static bool install_page(void *upage, void *kpage, bool writable);

/* Checks whether PHDR describes a valid, loadable segment in
   FILE and returns true if so, false otherwise. */
static bool
validate_segment(const struct Elf32_Phdr *phdr, struct file *file)
{
  /* p_offset and p_vaddr must have the same page offset. */
  if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK))
    return false;

  /* p_offset must point within FILE. */
  if (phdr->p_offset > (Elf32_Off)file_length(file))
    return false;

  /* p_memsz must be at least as big as p_filesz. */
  if (phdr->p_memsz < phdr->p_filesz)
    return false;

  /* The segment must not be empty. */
  if (phdr->p_memsz == 0)
    return false;

  /* The virtual memory region must both start and end within the
     user address space range. */
  if (!is_user_vaddr((void *)phdr->p_vaddr))
    return false;
  if (!is_user_vaddr((void *)(phdr->p_vaddr + phdr->p_memsz)))
    return false;

  /* The region cannot "wrap around" across the kernel virtual
     address space. */
  if (phdr->p_vaddr + phdr->p_memsz < phdr->p_vaddr)
    return false;

  /* Disallow mapping page 0.
     Not only is it a bad idea to map page 0, but if we allowed
     it then user code that passed a null pointer to system calls
     could quite likely panic the kernel by way of null pointer
     assertions in memcpy(), etc. */
  if (phdr->p_vaddr < PGSIZE)
    return false;

  /* It's okay. */
  return true;
}

/* Loads a segment starting at offset OFS in FILE at address
   UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
   memory are initialized, as follows:

        - READ_BYTES bytes at UPAGE must be read from FILE
          starting at offset OFS.

        - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.

   The pages initialized by this function must be writable by the
   user process if WRITABLE is true, read-only otherwise.

   Return true if successful, false if a memory allocation error
   or disk read error occurs. */
static bool
load_segment(struct file *file, off_t ofs, uint8_t *upage,
             uint32_t read_bytes, uint32_t zero_bytes, bool writable)
{
  ASSERT((read_bytes + zero_bytes) % PGSIZE == 0);
  ASSERT(pg_ofs(upage) == 0);
  ASSERT(ofs % PGSIZE == 0);

  file_seek(file, ofs);
  while (read_bytes > 0 || zero_bytes > 0)
  {
    /* Calculate how to fill this page.
       We will read PAGE_READ_BYTES bytes from FILE
       and zero the final PAGE_ZERO_BYTES bytes. */
    size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
    size_t page_zero_bytes = PGSIZE - page_read_bytes;

    /* Get a page of memory. */
    uint8_t *kpage = palloc_get_page(PAL_USER);
    if (kpage == NULL)
      return false;

    /* Load this page. */
    if (file_read(file, kpage, page_read_bytes) != (int)page_read_bytes)
    {
      palloc_free_page(kpage);
      return false;
    }
    memset(kpage + page_read_bytes, 0, page_zero_bytes);

    /* Add the page to the process's address space. */
    if (!install_page(upage, kpage, writable))
    {
      palloc_free_page(kpage);
      return false;
    }

    /* Advance. */
    read_bytes -= page_read_bytes;
    zero_bytes -= page_zero_bytes;
    upage += PGSIZE;
  }
  return true;
}

/* Create a minimal stack by mapping a zeroed page at the top of
   user virtual memory. */
static bool
setup_stack(void **esp)
{
  uint8_t *kpage;
  bool success = false;

  kpage = palloc_get_page(PAL_USER | PAL_ZERO);
  if (kpage != NULL)
  {
    success = install_page(((uint8_t *)PHYS_BASE) - PGSIZE, kpage, true);
    if (success)
      *esp = PHYS_BASE;
    else
      palloc_free_page(kpage);
  }
  return success;
}

/* Adds a mapping from user virtual address UPAGE to kernel
   virtual address KPAGE to the page table.
   If WRITABLE is true, the user process may modify the page;
   otherwise, it is read-only.
   UPAGE must not already be mapped.
   KPAGE should probably be a page obtained from the user pool
   with palloc_get_page().
   Returns true on success, false if UPAGE is already mapped or
   if memory allocation fails. */
static bool
install_page(void *upage, void *kpage, bool writable)
{
  struct thread *t = thread_current();

  /* Verify that there's not already a page at that virtual
     address, then map our page there. */
  return (pagedir_get_page(t->pagedir, upage) == NULL && pagedir_set_page(t->pagedir, upage, kpage, writable));
}
