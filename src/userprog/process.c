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
#include "userprog/syscall.h"
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/malloc.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "threads/synch.h"
#include "lib/string.h"
#include "vm/vm.h"
#include "vm/page.h"
#include "vm/frame.h"

static thread_func start_process NO_RETURN;
static bool load (const char *cmdline, void (**eip) (void), void **esp);
static void setup_stack_args(void **esp, char **argv, int argc);

/*
  struct child_status는 부모 프로세스가 갖고 있는 자식 프로세스의 상태 정보이다.
  부모 자식 간 동기화와 상태 전달의 핵심이 된다.
  자식이 종료될 경우 exit_code를 기록한다.
  부모가 wait 하고 있을 경우 sema_down 상태로 대기한다.
  이후 자식이 종료되면 exit_code를 회수한다. 
 */


/* Starts a new thread running a user program loaded from
   FILENAME.  The new thread may be scheduled (and may even exit)
   before process_execute() returns.  Returns the new process's
   thread id, or TID_ERROR if the thread cannot be created. */
tid_t
process_execute (const char *file_name) 
{
  char *fn_copy = NULL;
  tid_t tid;

  struct thread *parent = thread_current();
  struct child_status *cs = NULL;
  struct start_args *args = NULL;

  /* Make a copy of FILE_NAME.
     Otherwise there's a race between the caller and load(). */
  fn_copy = palloc_get_page (0);
  if (fn_copy == NULL)
    return TID_ERROR;
  strlcpy (fn_copy, file_name, PGSIZE);

  char *thread_name;
  char *save_ptr;
  // fn_copy를 직접 수정하면 start_process에 전달될 인자가 훼손되므로,
  // 파싱용 복사본을 하나 더 만든다. (더 효율적인 방법도 있지만 이것이 가장 안전함)
  char *parse_copy = palloc_get_page(0);
  if (parse_copy == NULL) {
      palloc_free_page(fn_copy);
      palloc_free_page(parse_copy);
      return TID_ERROR;
  }
  strlcpy(parse_copy, file_name, PGSIZE);
  thread_name = strtok_r(parse_copy, " ", &save_ptr);


  /*
    process_execute에서는 부모 - 자식 간 링크를 먼저 만들고 실행한다.
    부모가 소유하는 자식의 상태 노드를 cs라 하고(먼저 생성됨),
    이를 부모 컨테이너(children)에 넣(list push back)는다.
  */
  

  //자식의 상태를 나타내는 구조체 메모리를 할당하고 초기화한다.
  cs = malloc(sizeof *cs); //메모리 할당
  if (cs == NULL) 
    {
      palloc_free_page (fn_copy);
      palloc_free_page (parse_copy);
      return TID_ERROR;
    }
  //cs내부 요소 초기화 (아직 자식이 없는 상태이므로 초기값임)
  cs -> tid = TID_ERROR; 
  cs->exit_code = -1;
  cs->exited = false;
  cs->waited = false;
  cs->is_orphan = false;
  cs->load_ok = false;

  cs->is_alive = true;

  sema_init(&cs->load_sema, 0); //로드 초기화 용 세마포어
  sema_init(&cs->wait_sema, 0);
  
  args = malloc(sizeof *args);
  if (args == NULL)
    {
      palloc_free_page (fn_copy);
      palloc_free_page (parse_copy);
      free(cs);
      return TID_ERROR;
    }
  args-> cmdline = fn_copy;
  args-> cs = cs;

  lock_acquire(&parent->child_list_lock);
  list_push_back(&parent->children, &cs->elem);
  lock_release(&parent->child_list_lock);

  /* Create a new thread to execute FILE_NAME. */
  tid = thread_create (thread_name, PRI_DEFAULT, start_process, args);
  if (tid == TID_ERROR){
    // 스레드 생성 실패 시, 모든 할당된 자원을 정리
    lock_acquire(&parent->child_list_lock);
    list_remove(&cs->elem); // 리스트에서 제거
    lock_release(&parent->child_list_lock);

    free(cs);
    free(args);
    palloc_free_page (fn_copy);
    palloc_free_page (parse_copy);
    return TID_ERROR;
  }

  cs->tid = tid; // 생성된 자식 tid를 cs에 기록

  /* 3. 부모는 자식의 로드가 끝날 때까지 대기 */
  sema_down(&cs->load_sema);

  /* 5. 깨어난 후, 자식이 남긴 결과를 확인 */
  if (cs->load_ok) {
      palloc_free_page(parse_copy); // 성공 시 파싱용 복사본만 해제 
      return tid;
  } else {
      // 자식 로드 실패 시, 관련 정보 정리 후 에러 반환
      lock_acquire(&parent->child_list_lock);
      // list_remove는 이미 제거된 elem에 대해 패닉을 일으킬 수 있으므로,
      // cs->elem이 리스트에 있는지 확인하는 것이 더 안전함.
      // 여기서는 wait()가 호출되지 않았으므로 리스트에 있는 것이 확실함.
      list_remove(&cs->elem);
      lock_release(&parent->child_list_lock);
      
      free(cs);
      palloc_free_page (parse_copy);
      return TID_ERROR;
  }
}

/* A thread function that loads a user process and starts it
   running. 
   
  자식 프로세스가 시작 될 때 부모와의 연결 고리를 세팅하는 작업이
  start_process 내부에서 이루어져야 한다.
  원리는 다음과 같다.

  struct start_args를 우선 정의한다. (t)
  앞서 process_execute에서 만든 child_status *cs 포인터를 꺼내서
  현재 자식 스레드의 t-> as_child_in_parent 에 저장한다.
  이렇게 되면, as_child_in_parent는
  자식 프로세스 내부 구조체에서 부모의 위치를 가리키게 된다.

*/
static void
start_process (void *start_args_)
{
  struct start_args *args = start_args_;
  char *file_name = args->cmdline;
  struct child_status *cs = args -> cs;

  struct intr_frame if_;
  bool success;

  spt_init (&thread_current ()->spt);

  thread_current() -> as_child_in_parent = cs;

  /* Initialize interrupt frame and load executable. */
  memset (&if_, 0, sizeof if_);
  if_.gs = if_.fs = if_.es = if_.ds = if_.ss = SEL_UDSEG;
  if_.cs = SEL_UCSEG;
  if_.eflags = FLAG_IF | FLAG_MBS;
  success = load (file_name, &if_.eip, &if_.esp);

  cs->load_ok = success;
  sema_up(&cs->load_sema);

  palloc_free_page (file_name);
  free(args);
  /* If load failed, quit. */
  if (!success){ 
    thread_current()->as_child_in_parent = NULL;
    thread_exit ();
  }
  /* Start the user process by simulating a return from an
     interrupt, implemented by intr_exit (in
     threads/intr-stubs.S).  Because intr_exit takes all of its
     arguments on the stack in the form of a `struct intr_frame',
     we just point the stack pointer (%esp) to our stack frame
     and jump to it. */
  asm volatile ("movl %0, %%esp; jmp intr_exit" : : "g" (&if_) : "memory");
  NOT_REACHED ();
}

/* Waits for thread TID to die and returns its exit status.  If
   it was terminated by the kernel (i.e. killed due to an
   exception), returns -1.  If TID is invalid or if it was not a
   child of the calling process, or if process_wait() has already
   been successfully called for the given TID, returns -1
   immediately, without waiting.

   This function will be implemented in problem 2-2.  For now, it
   does nothing. */

int
process_wait (tid_t child_tid) 
{
  struct thread *cur = thread_current ();
  struct child_status *cs = NULL;

  lock_acquire(&cur->child_list_lock);
  
  struct list_elem *e;
  for (e = list_begin (&cur->children); e != list_end (&cur->children); e = list_next (e))
    {
      struct child_status *temp_cs = list_entry (e, struct child_status, elem);
      if (temp_cs->tid == child_tid)
        {
          cs = temp_cs;
          break;
        }
    }

  if (cs == NULL || cs->waited)
    {
      lock_release(&cur->child_list_lock); 
      return -1;
    }
  
  cs->waited = true;

  list_remove(&cs->elem);
  
  lock_release(&cur->child_list_lock);

  if (!cs->exited)
    {
      sema_down (&cs->wait_sema);
    }

  int exit_code = cs->exit_code;
  free (cs);

  return exit_code;
}



/* Free the current process's resources. */
void
process_exit (void)
{
  struct thread *cur = thread_current ();
  uint32_t *pd;

  printf("%s: exit(%d)\n", cur->name,cur->exit_status);

  while (!list_empty(&cur->mmap_list)) {
      struct list_elem *e = list_begin(&cur->mmap_list);
      struct mmap_file *mmap_info = list_entry(e, struct mmap_file, elem);
      
      /* do_munmap 내부에서 리스트 제거(list_remove)와 free를 수행함 */
      do_munmap(mmap_info);
  }

  if (cur->as_child_in_parent != NULL) {
      struct child_status *cs = cur->as_child_in_parent;
      
      cs->is_alive = false;
      cs->exit_code = cur->exit_status;
      cs->exited = true;

      if (cs->is_orphan) {

          free(cs);
      } else {

          sema_up(&cs->wait_sema);
      }
  }

  lock_acquire(&cur->child_list_lock);
  
  struct list_elem *e = list_begin(&cur->children);
  while (!list_empty(&cur->children)) {    
      struct child_status *cs = list_entry(e, struct child_status, elem);

      if (!cs->is_alive) {
          e = list_remove(e);
          free(cs);
      } else {
          cs->is_orphan = true;
          e = list_next(e);
      }
  }
  lock_release(&cur->child_list_lock);
  if (lock_held_by_current_thread(&filesys_lock)) {
    lock_release(&filesys_lock);
  }

  lock_acquire(&filesys_lock);

  if (cur->where_thread_came_from != NULL) {
      file_allow_write(cur->where_thread_came_from);
      file_close(cur->where_thread_came_from);
      cur->where_thread_came_from = NULL; // 안전을 위해 NULL 처리
  }

  for (int i = 2; i < 128; i++) {
      if (cur->file_descripor[i] != NULL) { 
          file_close(cur->file_descripor[i]);
          cur->file_descripor[i] = NULL;
      }
  }
  
  lock_release(&filesys_lock);

  spt_kill(&cur->spt);
  pd = cur->pagedir;
  if (pd != NULL) {
      cur->pagedir = NULL;
      pagedir_activate(NULL);
      pagedir_destroy(pd);
  }
}

  /* Destroy the current process's page directory and switch back
     to the kernel-only page directory. */
   
        /* Correct ordering here is crucial.  We must set
            cur->pagedir to NULL before switching page directories,
            so that a timer interrupt can't switch back to the
            process page directory.  We must activate the base page
            directory before destroying the process's page
            directory, or our active page directory will be one
            that's been freed (and cleared). */
       // cur->pagedir = NULL;
        //pagedir_activate (NULL);
        //pagedir_destroy (pd);
     

/* Sets up the CPU for running user code in the current
   thread.
   This function is called on every context switch. */
void
process_activate (void)
{
  struct thread *t = thread_current ();

  /* Activate thread's page tables. */
  pagedir_activate (t->pagedir);

  /* Set thread's kernel stack for use in processing
     interrupts. */
  tss_update ();
}

/* We load ELF binaries.  The following definitions are taken
   from the ELF specification, [ELF1], more-or-less verbatim.  */

/* ELF types.  See [ELF1] 1-2. */
typedef uint32_t Elf32_Word, Elf32_Addr, Elf32_Off;
typedef uint16_t Elf32_Half;

/* For use with ELF types in printf(). */
#define PE32Wx PRIx32   /* Print Elf32_Word in hexadecimal. */
#define PE32Ax PRIx32   /* Print Elf32_Addr in hexadecimal. */
#define PE32Ox PRIx32   /* Print Elf32_Off in hexadecimal. */
#define PE32Hx PRIx16   /* Print Elf32_Half in hexadecimal. */

/* Executable header.  See [ELF1] 1-4 to 1-8.
   This appears at the very beginning of an ELF binary. */
struct Elf32_Ehdr
  {
    unsigned char e_ident[16];
    Elf32_Half    e_type;
    Elf32_Half    e_machine;
    Elf32_Word    e_version;
    Elf32_Addr    e_entry;
    Elf32_Off     e_phoff;
    Elf32_Off     e_shoff;
    Elf32_Word    e_flags;
    Elf32_Half    e_ehsize;
    Elf32_Half    e_phentsize;
    Elf32_Half    e_phnum;
    Elf32_Half    e_shentsize;
    Elf32_Half    e_shnum;
    Elf32_Half    e_shstrndx;
  };

/* Program header.  See [ELF1] 2-2 to 2-4.
   There are e_phnum of these, starting at file offset e_phoff
   (see [ELF1] 1-6). */
struct Elf32_Phdr
  {
    Elf32_Word p_type;
    Elf32_Off  p_offset;
    Elf32_Addr p_vaddr;
    Elf32_Addr p_paddr;
    Elf32_Word p_filesz;
    Elf32_Word p_memsz;
    Elf32_Word p_flags;
    Elf32_Word p_align;
  };

/* Values for p_type.  See [ELF1] 2-3. */
#define PT_NULL    0            /* Ignore. */
#define PT_LOAD    1            /* Loadable segment. */
#define PT_DYNAMIC 2            /* Dynamic linking info. */
#define PT_INTERP  3            /* Name of dynamic loader. */
#define PT_NOTE    4            /* Auxiliary info. */
#define PT_SHLIB   5            /* Reserved. */
#define PT_PHDR    6            /* Program header table. */
#define PT_STACK   0x6474e551   /* Stack segment. */

/* Flags for p_flags.  See [ELF3] 2-3 and 2-4. */
#define PF_X 1          /* Executable. */
#define PF_W 2          /* Writable. */
#define PF_R 4          /* Readable. */

static bool setup_stack (void **esp);
static bool validate_segment (const struct Elf32_Phdr *, struct file *);
static bool load_segment (struct file *file, off_t ofs, uint8_t *upage,
                          uint32_t read_bytes, uint32_t zero_bytes,
                          bool writable);

/* Loads an ELF executable from FILE_NAME into the current thread.
   Stores the executable's entry point into *EIP
   and its initial stack pointer into *ESP.
   Returns true if successful, false otherwise. */
bool
load (const char *file_name, void (**eip) (void), void **esp) 
{
  struct thread *t = thread_current ();
  struct Elf32_Ehdr ehdr;
  struct file *file = NULL; // in Project 2!
  off_t file_ofs;
  bool success = false;
  int i;

  /* Allocate and activate page directory. */
  t->pagedir = pagedir_create ();
  if (t->pagedir == NULL) 
    goto done;
  process_activate ();

  /* Open executable file. */
  /*여기에서 file_name을 인자로 받는데, file_name은 명령줄로 입력한 한 줄 전체이다.
    따라서 이를 적절히 수정해야 한다.
  */

 /* Argument Passing을 위한 file_name tokenize*/
  #define MAX_ARGC 64
  size_t str_bytes = 0;
  char *fn_copy = NULL;
  fn_copy = palloc_get_page (0);
  if (fn_copy == NULL)
    goto done;  // 메모리 부족 시 실패 처리
  strlcpy(fn_copy, file_name, PGSIZE);

  char *token, *save_ptr = NULL;
  int argc = 0;
  char *argv[MAX_ARGC];

  token = strtok_r(fn_copy," ",&save_ptr);
  //cut file_name, which is divided by " " . Use strtok.

  while(token != NULL){
    size_t len = strlen(token) + 1;
    if (argc >= MAX_ARGC) {
    printf("load: too many arguments (>%d)\n", MAX_ARGC);
    goto done;
    }
    size_t new_str_bytes = str_bytes + len;
    size_t padding        = (4 - (new_str_bytes % 4)) % 4;
    size_t pointers_bytes =
      ( (argc + 2) * sizeof(char *) )   
      + sizeof(char **)           
      + sizeof(int)                      
      + sizeof(void *);   

    if (new_str_bytes + padding + pointers_bytes > PGSIZE) {
      printf("load: arguments exceed 1 page (4KB)\n");
      goto done;
    }

    argv[argc] = token;
    argc++;
    str_bytes = new_str_bytes;
    token = strtok_r(NULL," ",&save_ptr);
  }

  if (argc == 0) {                             // ➐ 빈 명령줄 방어
  printf("load: empty command line\n");
  goto done;
  }
  //put the tokens each, to the argv array. also add 1 to argc at that time.
  //after escaping the loop, argv array will be filled with tokens, and argc will be the number of tokens.
  const char *prog = argv[0];
  //이제 prog는 argv[0], 즉 진짜 '파일 이름'을 가리키게 된다.


  //Project 2 : File system 구현을 위한 load()내 수정

  lock_acquire(&filesys_lock);
  file = filesys_open (prog);
  lock_release(&filesys_lock);
  
  if (file == NULL) 
    {
      printf ("load: %s: open failed\n", prog);
      goto done; 
    }

  thread_current()->where_thread_came_from = file;
  file_deny_write(file);


  /* Read and verify executable header. */
  if (file_read (file, &ehdr, sizeof ehdr) != sizeof ehdr
      || memcmp (ehdr.e_ident, "\177ELF\1\1\1", 7)
      || ehdr.e_type != 2
      || ehdr.e_machine != 3
      || ehdr.e_version != 1
      || ehdr.e_phentsize != sizeof (struct Elf32_Phdr)
      || ehdr.e_phnum > 1024) 
    {
      printf ("load: %s: error loading executable\n", prog);
      goto done; 
    }

  /* Read program headers. */
  file_ofs = ehdr.e_phoff;
  for (i = 0; i < ehdr.e_phnum; i++) 
    {
      struct Elf32_Phdr phdr;

      if (file_ofs < 0 || file_ofs > file_length (file))
        goto done;
      file_seek (file, file_ofs);

      if (file_read (file, &phdr, sizeof phdr) != sizeof phdr)
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
          if (validate_segment (&phdr, file)) 
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
                  zero_bytes = (ROUND_UP (page_offset + phdr.p_memsz, PGSIZE)
                                - read_bytes);
                }
              else 
                {
                  /* Entirely zero.
                     Don't read anything from disk. */
                  read_bytes = 0;
                  zero_bytes = ROUND_UP (page_offset + phdr.p_memsz, PGSIZE);
                }
              if (!load_segment (file, file_page, (void *) mem_page,
                                 read_bytes, zero_bytes, writable))
                goto done;
            }
          else
            goto done;
          break;
        }
    }

  

  /* Set up stack. */
  if (!setup_stack (esp))
    goto done;

  setup_stack_args(esp,argv,argc);

  //hex_dump((uintptr_t)*esp, *esp, (size_t)((uint8_t*)PHYS_BASE - (uint8_t*)(*esp)), true);



  /* Start address. */
  *eip = (void (*) (void)) ehdr.e_entry;

  success = true;
  
 done:
  /* We arrive here whether the load is successful or not. */
  if (fn_copy!=NULL) palloc_free_page(fn_copy);

  if (!success && file != NULL)
    {
      thread_current()->where_thread_came_from = NULL; 
      file_close(file);
    }
  
  return success;

}

/* load() helpers. */

static bool install_page (void *upage, void *kpage, bool writable);

/* Checks whether PHDR describes a valid, loadable segment in
   FILE and returns true if so, false otherwise. */
static bool
validate_segment (const struct Elf32_Phdr *phdr, struct file *file) 
{
  /* p_offset and p_vaddr must have the same page offset. */
  if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK)) 
    return false; 

  /* p_offset must point within FILE. */
  if (phdr->p_offset > (Elf32_Off) file_length (file)) 
    return false;

  /* p_memsz must be at least as big as p_filesz. */
  if (phdr->p_memsz < phdr->p_filesz) 
    return false; 

  /* The segment must not be empty. */
  if (phdr->p_memsz == 0)
    return false;
  
  /* The virtual memory region must both start and end within the
     user address space range. */
  if (!is_user_vaddr ((void *) phdr->p_vaddr))
    return false;
  if (!is_user_vaddr ((void *) (phdr->p_vaddr + phdr->p_memsz)))
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
  

struct lazy_load_arg {
    struct file *file;
    off_t ofs;
    uint32_t read_bytes;
    uint32_t zero_bytes;
};

extern struct lock filesys_lock;

static bool
lazy_load_segment (struct page *page, enum vm_type type UNUSED, void *aux) {
    struct mmap_info_aux *info = (struct mmap_info_aux *)aux;
    
    /* 1. 파일 데이터 읽기 */
    if (info->file == NULL) return false;

    // Load 시점에는 프레임이 할당되어 있습니다 (vm_do_claim_page 과정 중)
    lock_acquire(&filesys_lock);
    off_t read_bytes = file_read_at(info->file, page->frame->kva, 
                                    info->read_bytes, info->ofs);
    lock_release(&filesys_lock);

    if (read_bytes != (int)info->read_bytes) {
        return false;
    }

    /* 2. 나머지 공간 0으로 초기화 */
    memset(page->frame->kva + info->read_bytes, 0, info->zero_bytes);

    /* 3. 리소스 정리 (파일 닫기) */
    /* 이제 메모리에 다 읽었으므로 파일 핸들은 필요 없습니다. */
    file_close(info->file);
    free(info);

    /* 4. [핵심] 페이지 전환 (File Backed -> Anon) */
    /* 이제부터 이 페이지는 파일과 인연을 끊고, 스왑 디스크와 소통하는 익명 페이지가 됩니다. */
    /* 이를 위해 anon_initializer를 호출하여 operations를 anon_ops로 바꿔줍니다. */
    return anon_initializer(page, VM_ANON, NULL);
}

/* [수정 2] load_segment 함수 */
static bool
load_segment (struct file *file, off_t ofs, uint8_t *upage,
              uint32_t read_bytes, uint32_t zero_bytes, bool writable) 
{
  ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
  ASSERT (pg_ofs (upage) == 0);
  ASSERT (ofs % PGSIZE == 0);

  while (read_bytes > 0 || zero_bytes > 0) 
  {
      size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
      size_t page_zero_bytes = PGSIZE - page_read_bytes;

      struct mmap_info_aux *aux = malloc(sizeof(struct mmap_info_aux));
      if (aux == NULL)
          return false;

      aux->file = file_reopen(file);
      if (aux->file == NULL) {
          free(aux);
          return false;
      }

      aux->ofs = ofs;
      aux->read_bytes = page_read_bytes;
      aux->zero_bytes = page_zero_bytes;

      if (!vm_alloc_page_with_initializer (VM_ANON, upage,
              writable, lazy_load_segment, aux)) {
          file_close(aux->file);
          free(aux);
          return false;
      }

      read_bytes -= page_read_bytes;
      zero_bytes -= page_zero_bytes;
      upage += PGSIZE;
      ofs += page_read_bytes; 
  }
  return true;
}

static bool
setup_stack (void **esp) 
{
  bool success = false;
  void *stack_bottom = (void *) (((uint8_t *) PHYS_BASE) - PGSIZE);

  if (vm_alloc_page (VM_ANON | VM_MARKER_0, stack_bottom, true)) 
    {
      success = vm_claim_page (stack_bottom);

      if (success) 
        {
          *esp = PHYS_BASE;
        }
        else{
        }
    }
    else
    {
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
install_page (void *upage, void *kpage, bool writable)
{
  struct thread *t = thread_current ();

  /* Verify that there's not already a page at that virtual
     address, then map our page there. */
  return (pagedir_get_page (t->pagedir, upage) == NULL
          && pagedir_set_page (t->pagedir, upage, kpage, writable));
}

static void
setup_stack_args (void **esp, char **argv, int argc)
{
  //인자 문자열들을 스택에 복사
  char *arg_addrs[64];
  ASSERT (argc >= 0 && argc <= 64);

  for (int i = argc - 1; i >= 0; i--) {
    size_t len = strlen(argv[i]) + 1;
    *esp = (char *)*esp - len;
    memcpy(*esp, argv[i], len);
    arg_addrs[i] = *esp;
  }

  //스택 포인터 4바이트 정렬
  *esp = (void *)((uintptr_t)*esp & ~3);

  //main 함수 인자 프레임 배치
  void **p_esp = (void **)*esp;

  //argv 배열의 끝 표시
  *--p_esp = NULL;

  for (int i = argc - 1; i >= 0; i--) {
    *--p_esp = arg_addrs[i];
  }

  void **argv_start_addr = p_esp;
  
  *--p_esp = argv_start_addr;

  *--p_esp = (void *)argc;
  *--p_esp = NULL;
  *esp = p_esp;
}
