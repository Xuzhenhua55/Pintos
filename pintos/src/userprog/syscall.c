#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "string.h"
#include "threads/vaddr.h"
#include "filesys/filesys.h"
#include "filesys/file.h"
#include "filesys/inode.h"
#include "filesys/directory.h"
static void syscall_handler(struct intr_frame *);
static void syscall_halt(struct intr_frame *) NO_RETURN;
static void syscall_exit(struct intr_frame *) NO_RETURN;
static void syscall_exec(struct intr_frame *);
static void syscall_wait(struct intr_frame *);

static void syscall_create(struct intr_frame *);
static void syscall_remove(struct intr_frame *);
static void syscall_open(struct intr_frame *);
static void syscall_filesize(struct intr_frame *);
static void syscall_read(struct intr_frame *);
static void syscall_write(struct intr_frame *);
static void syscall_seek(struct intr_frame *);
static void syscall_tell(struct intr_frame *);
static void syscall_close(struct intr_frame *);

bool syscall_chdir(struct intr_frame *f);
bool syscall_mkdir(struct intr_frame *f);
bool syscall_readdir(struct intr_frame *f);
bool syscall_isdir(struct intr_frame *f);
int syscall_inumber(struct intr_frame *f);

static void *check_read_user_ptr(const void *, size_t);
static void *check_write_user_ptr(void *, size_t);
static char *check_read_user_str(const char *);

static int get_user(const uint8_t *uaddr);
static bool put_user(uint8_t *udst, uint8_t byte);

static void terminate_process(void);
static struct file_entry *get_file_by_fd(int fd);
static size_t ptr_size = sizeof(void *);

void syscall_init(void)
{
  intr_register_int(0x30, 3, INTR_ON, syscall_handler, "syscall");
}

// 在用户的中断处理程序中解析系统调用的符号
// 根据类型分发给各个具体处理函数逐个实现
static void
syscall_handler(struct intr_frame *f UNUSED)
{
  int syscall_type = *(int *)check_read_user_ptr(f->esp, sizeof(int));
  switch (syscall_type)
  {
  case SYS_HALT:
    syscall_halt(f);
    break;
  case SYS_EXIT:
    syscall_exit(f);
    break;
  case SYS_EXEC:
    syscall_exec(f);
    break;
  case SYS_WAIT:
    syscall_wait(f);
    break;
  case SYS_CREATE:
    syscall_create(f);
    break;
  case SYS_REMOVE:
    syscall_remove(f);
    break;
  case SYS_OPEN:
    syscall_open(f);
    break;
  case SYS_FILESIZE:
    syscall_filesize(f);
    break;
  case SYS_READ:
    syscall_read(f);
    break;
  case SYS_WRITE:
    syscall_write(f);
    break;
  case SYS_SEEK:
    syscall_seek(f);
    break;
  case SYS_TELL:
    syscall_tell(f);
    break;
  case SYS_CLOSE:
    syscall_close(f);
    break;
  case SYS_CHDIR:
    syscall_chdir(f);
    break;
  case SYS_MKDIR:
    syscall_mkdir(f);
    break;
  case SYS_READDIR:
    syscall_readdir(f);
    break;
  case SYS_ISDIR:
    syscall_isdir(f);
    break;
  case SYS_INUMBER:
    syscall_inumber(f);
    break;
  default:
    NOT_REACHED();
    break;
  }
}

// 强制退出Pintos`halt`
static void
syscall_halt(struct intr_frame *f UNUSED)
{
  shutdown_power_off();
}

static void
syscall_exit(struct intr_frame *f)
{
  // exit_code在系统调用之后被解析为参数
  int exit_code = *(int *)check_read_user_ptr(f->esp + ptr_size, sizeof(int));
  thread_current()->exit_code = exit_code;
  thread_exit();
}
// 运行其名称在 cmd_line 中给出的可执行文件，并传递任何给定的参数，返回新进程的进程ID(pid)
static void
syscall_exec(struct intr_frame *f)
{
  char *cmd_line = *(char **)check_read_user_ptr(f->esp + ptr_size, ptr_size);
  check_read_user_str(cmd_line);
  f->eax = process_execute(cmd_line);
}
// 获取pid参数并调用process_wait
static void
syscall_wait(struct intr_frame *f)
{
  int pid = *(int *)check_read_user_ptr(f->esp + ptr_size, sizeof(int));
  f->eax = process_wait(pid);
}
// 创建一个名为file的新文件，其初始大小为 initial_size个字节。如果成功，则返回true，否则返回false。创建新文件不会打开它：打开新文件是一项单独的操作，需要系统调用“open”。
// 修改filesys_create的调用
static void
syscall_create(struct intr_frame *f)
{
  char *file_name = *(char **)check_read_user_ptr(f->esp + ptr_size, ptr_size);
  check_read_user_str(file_name);
  unsigned file_size = *(unsigned *)check_read_user_ptr(f->esp + 2 * ptr_size, sizeof(unsigned));

  lock_acquire(&filesys_lock);
  bool res = filesys_create(file_name, file_size, false);
  f->eax = res;
  lock_release(&filesys_lock);
}
// 删除名为file的文件。如果成功，则返回true，否则返回false。不论文件是打开还是关闭，都可以将其删除，并且删除打开的文件不会将其关闭。
static void
syscall_remove(struct intr_frame *f)
{
  // 解析参数获取file
  char *file_ = *(char **)check_read_user_ptr(f->esp + ptr_size, ptr_size);
  check_read_user_str(file_); // 对file进行字符串的访存检查

  lock_acquire(&filesys_lock);
  f->eax = filesys_remove(file_); // 将file文件删除同时将结果返回给eax
  lock_release(&filesys_lock);
}
// 打开名为 file 的文件， 返回一个称为"文件描述符"(fd)的非负整数句柄；如果无法打开文件，则返回-1.
static void
syscall_open(struct intr_frame *f)
{
  // 获取参数“文件名”
  char *file_name = *(char **)check_read_user_ptr(f->esp + ptr_size, ptr_size);
  check_read_user_str(file_name);
  // 根据文件名打开文件
  lock_acquire(&filesys_lock);
  struct file *opened_file = filesys_open(file_name);
  lock_release(&filesys_lock);
  // 如果文件不存在则返回
  if (opened_file == NULL)
  {
    f->eax = -1;
    return;
  }
  // 将该文件添加给当前当前进行管理
  struct thread *current_thread = thread_current();
  struct file_entry *entry = malloc(sizeof(struct file_entry));
  entry->fd = current_thread->next_fd++;
  entry->f = opened_file;
  list_push_back(&current_thread->file_list, &entry->elem);
  f->eax = entry->fd;
}
// 返回以fd打开的文件的大小（以字节为单位）
// 增加判断文件是否是目录
static void
syscall_filesize(struct intr_frame *f)
{
  // 解析参数获得fd
  int fd = *(int *)check_read_user_ptr(f->esp + ptr_size, sizeof(int));
  // 根据fd获得文件指针
  struct file_entry *entry = get_file_by_fd(fd);
  if (entry->f == NULL)
  {
    f->eax = -1;
  }
  else
  {
    lock_acquire(&filesys_lock);
    if (inode_is_dir(file_get_inode(entry->f)))
    {
      f->eax = -1;
    }
    else
    {
      f->eax = file_length(entry->f);
    }
    lock_release(&filesys_lock);
  }
}
// 从打开为fd的文件中读取size个字节到buffer中。返回实际读取的字节数（文件末尾为0），如果无法读取文件（由于文件末尾以外的条件），则返回-1。 fd 0使用input_getc()从键盘读取。
// 增加判断读取的文件不是目录
static void
syscall_read(struct intr_frame *f)
{
  // 解析参数获得fd、buf和size
  int fd = *(int *)check_read_user_ptr(f->esp + ptr_size, sizeof(int));
  void *buf = *(void **)check_read_user_ptr(f->esp + 2 * ptr_size, ptr_size);
  unsigned size = *(int *)check_read_user_ptr(f->esp + 3 * ptr_size, sizeof(unsigned));
  check_write_user_ptr(buf, size); // 对长度为size的buf进行指针校验
  // 如果fd为0那么使用input_gec()从键盘中读取
  if (fd == 0)
  {
    for (size_t i = 0; i < size; i++)
    {
      *(uint8_t *)buf = input_getc();
      buf += sizeof(uint8_t);
    }
    f->eax = size;
    return;
  }
  // 如果fd为1意味着向STDOUT中读，这是不合理的
  if (fd == 1)
  {
    terminate_process();
  }
  // 以下情况为从文件中进行读取
  // 根据fd获取文件指针
  struct file_entry *entry = get_file_by_fd(fd);
  if (entry != NULL)
  {
    lock_acquire(&filesys_lock);

    if (inode_is_dir(file_get_inode(entry->f)))
    {
      f->eax = -1;
    }
    else
    {
      f->eax = file_read(entry->f, buf, size); // 如果entry不为NULL那么将size个字节读入buf同时返回size给eax
    }
    lock_release(&filesys_lock);
  }
  else
  {
    f->eax = -1;
  }
}
// 适应写调用：将size个字节从buffer写入打开的文件fd返回实际读取的字节数（文件末尾为0）
// 如果无法读取文件（由于文件末尾以外的条件），则返回-1；fd 0使用input_getc()从键盘读取
// 增加判断写入的文件不是目录
static void
syscall_write(struct intr_frame *f)
{
  // 解析参数获得fd、buf和size
  int fd = *(int *)check_read_user_ptr(f->esp + ptr_size, sizeof(int));
  void *buf = *(void **)check_read_user_ptr(f->esp + 2 * ptr_size, ptr_size);
  unsigned size = *(int *)check_read_user_ptr(f->esp + 3 * ptr_size, sizeof(unsigned));
  check_read_user_ptr(buf, size); // 对长度为size的buf进行指针校验
  // 如果fd为0那么意味着向STDIN中写，这是不合理的
  if (fd == 0)
  {
    terminate_process();
  }
  // 如果fd为1那么写入控制台
  if (fd == 1)
  {
    putbuf((char *)buf, size);
    f->eax = size;
    return;
  }
  // 以下情况为向文件中写入
  // 根据fd获取文件指针
  struct file_entry *entry = get_file_by_fd(fd);
  ;
  if (entry != NULL)
  {
    lock_acquire(&filesys_lock);
    if (inode_is_dir(file_get_inode(entry->f)))
    {
      f->eax = -1;
    }
    else
    {
      f->eax = file_write(entry->f, buf, size); // 如果entry不为NULL那么将size个字节写入buf并返回size
    }
    lock_release(&filesys_lock);
  }
  else
  {
    f->eax = -1;
  }
}
// 将打开文件fd中要读取或写入的下一个字节更改为position，以从文件开头开始的字节表示。（因此，position为0是文件的开始。）
// 增加判断文件不是目录
static void
syscall_seek(struct intr_frame *f)
{
  // 解析参数获得fd和pos
  int fd = *(int *)check_read_user_ptr(f->esp + ptr_size, sizeof(int));
  unsigned pos = *(int *)check_read_user_ptr(f->esp + 2 * ptr_size, sizeof(unsigned));
  // 根据fd获得文件指针
  struct file_entry *entry = get_file_by_fd(fd);
  if (entry != NULL)
  {
    lock_acquire(&filesys_lock);
    // 如果文件指针不为NULL同时file也不为null且file不是目录那么就将fd中要读取或者写入的下一个字节更改为position
    if (entry->f != NULL)
    {
      if (!inode_is_dir(file_get_inode(entry->f)))
        file_seek(entry->f, pos);
    }
    lock_release(&filesys_lock);
  }
}
// 返回打开文件fd中要读取或写入的下一个字节的位置，以从文件开头开始的字节数表示。
// 增加判断文件不是目录
static void
syscall_tell(struct intr_frame *f)
{
  // 解析参数获得fd
  int fd = *(int *)check_read_user_ptr(f->esp + ptr_size, sizeof(int));
  // 根据fd获得文件指针
  struct file_entry *entry = get_file_by_fd(fd);
  if (entry != NULL)
  {
    lock_acquire(&filesys_lock);
    if (entry->f != NULL)
    {
      if (inode_is_dir(file_get_inode(entry->f)))
      {
        f->eax = -1;
      }
      else
      {
        f->eax = file_tell(entry->f); // 如果文件指针不为NULL那么就返回要读取或写入的下一个字节的位置
      }
    }
    else
    {
      f->eax = -1;
    }
    lock_release(&filesys_lock);
  }
  else
  {
    f->eax = -1;
  }
  return f->eax;
}
// 关闭文件描述符fd。退出或终止进程会隐式关闭其所有打开的文件描述符，就像通过为每个进程调用此函数一样。
static void
syscall_close(struct intr_frame *f)
{
  // 解析参数获得fd
  int fd = *(int *)check_read_user_ptr(f->esp + ptr_size, sizeof(int));
  // 根据fd获得文件指针
  struct file_entry *entry = get_file_by_fd(fd);
  if (entry != NULL)
  {
    lock_acquire(&filesys_lock);

    file_close(entry->f); // 将fd关闭
    if (entry->f = NULL)
    {
      // 根据file获取inode
      struct inode *inode = file_get_inode(entry->f);
      // 如果inode为null那么进入下一轮循环
      if (inode == NULL)
        return;
      // 如果inode为目录那么以目录形式关闭
      if (inode_is_dir(inode))
      {
        dir_close(entry->f);
      } // 如果inode为文件那么以文件形式关闭
      else
      {
        file_close(entry->f);
      }
    }
    list_remove(&entry->elem); // 将fd从含有它的列表中移除
    free(entry);               // 将该入口所占有的空间释放
    lock_release(&filesys_lock);
  }
}
// 根据指定的path来修改当前线程的目录
bool syscall_chdir(struct intr_frame *f)
{
  char *path = *(char **)check_read_user_ptr(f->esp + ptr_size, ptr_size);
  bool success = filesys_chdir(path);
  f->eax = success;
  return success;
}
// 根据指定的path来创建目录
bool syscall_mkdir(struct intr_frame *f)
{
  char *path = *(char **)check_read_user_ptr(f->esp + ptr_size, ptr_size);
  bool success = filesys_create(path, 0, true);
  f->eax = success;
  return success;
}
// 将指定dir目录下的下一个已使用的dir_entry的名称赋值给path
bool syscall_readdir(struct intr_frame *f)
{
  f->eax = false;
  int fd = *(int *)check_read_user_ptr(f->esp + ptr_size, sizeof(int));
  char *name = *(char **)check_read_user_ptr(f->esp + 2 * ptr_size, ptr_size);
  struct file_entry *entry = get_file_by_fd(fd);
  struct file *file = entry->f;
  if (file == NULL)
    return false;
  struct inode *inode = file_get_inode(file);
  if (inode == NULL)
    return false;
  if (!inode_is_dir(inode))
    return false;
  struct dir *dir = (struct dir *)file;
  if (!dir_readdir(dir, name))
    return false;
  f->eax = true;
  return true;
}
// 判断某个文件是否是目录
bool syscall_isdir(struct intr_frame *f)
{
  f->eax = false;
  int fd = *(int *)check_read_user_ptr(f->esp + ptr_size, sizeof(int));
  struct file_entry *entry = get_file_by_fd(fd);
  struct file *file = entry->f;
  if (file == NULL)
    return false;
  struct inode *inode = file_get_inode(file);
  if (inode == NULL)
    return false;
  if (!inode_is_dir(inode))
    return false;
  f->eax = true;
  return true;
}
// 获取某个文件所占据的扇区本身
int syscall_inumber(struct intr_frame *f)
{
  f->eax = -1;
  int fd = *(int *)check_read_user_ptr(f->esp + ptr_size, sizeof(int));
  struct file_entry *entry = get_file_by_fd(fd);
  struct file *file = entry->f;
  if (file == NULL)
    return -1;
  struct inode *inode = file_get_inode(file);
  if (inode == NULL)
    return -1;
  block_sector_t inumber = inode_get_inumber(inode);
  f->eax = inumber;
  return inumber;
}

// 从用户虚拟地址空间中读取一个字节的信息，如果成功那么就返回该信息否则返回-1
static int
get_user(const uint8_t *uaddr)
{
  int result;
  asm("movl $1f, %0; movzbl %1, %0; 1:"
      : "=&a"(result)
      : "m"(*uaddr));
  return result;
}
// 向用户地址空间写入一字节的信息如果成功就返回true否则返回false
static bool
put_user(uint8_t *udst, uint8_t byte)
{
  int error_code;
  asm("movl $1f, %0; movb %b2, %1; 1:"
      : "=&a"(error_code), "=m"(*udst)
      : "q"(byte));
  return error_code != -1;
}

// 检查一个用户提供的指针是否能够合法读取数据，如果合法就返回该指针否则就调用terminate_process
static void *
check_read_user_ptr(const void *ptr, size_t size)
{
  if (!is_user_vaddr(ptr))
  {
    terminate_process();
  }
  for (size_t i = 0; i < size; i++)
  { // check if every byte is safe to read
    if (get_user(ptr + i) == -1)
    {
      terminate_process();
    }
  }
  return (void *)ptr; // remove const
}

// 检查一个用户提供的指针是否能够合法写数据，如果合法就返回该指针否则就调用terminate_process
static void *
check_write_user_ptr(void *ptr, size_t size)
{
  if (!is_user_vaddr(ptr))
  {
    terminate_process();
  }
  for (size_t i = 0; i < size; i++)
  {
    if (!put_user(ptr + i, 0))
    { // check if every byte is safe to write
      terminate_process();
    }
  }
  return ptr;
}
// 检查一个用户提供的字符串是否能够合法写数据，如果合法就返回该字符串否则就调用terminate_process
static char *
check_read_user_str(const char *str)
{
  if (!is_user_vaddr(str))
  {
    terminate_process();
  }

  uint8_t *_str = (uint8_t *)str;
  while (true)
  {
    int c = get_user(_str);
    if (c == -1)
    {
      terminate_process();
    }
    else if (c == '\0')
    {                     // reached the end of str
      return (char *)str; // remove const
    }
    ++_str;
  }
  NOT_REACHED();
}
// 终止一个进程
static void
terminate_process(void)
{
  thread_current()->exit_code = -1;
  thread_exit();
  NOT_REACHED();
}

// 根据fd来查找当前线程是否管理着文件描述符为fd的文件，如果存在那么返回file_entry*否则就返回NULL
static struct file_entry *
get_file_by_fd(int fd)
{
  struct thread *current_thread = thread_current();
  struct list_elem *elem_;
  for (elem_ = list_begin(&current_thread->file_list); elem_ != list_end(&current_thread->file_list);
       elem_ = list_next(elem_))
  {
    struct file_entry *entry = list_entry(elem_, struct file_entry, elem);
    if (entry->fd == fd)
    {
      return entry;
    }
  }
  return NULL;
}
