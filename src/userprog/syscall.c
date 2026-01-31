#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/palloc.h"
#include "threads/synch.h"

#include "filesys/file.h"
#include "filesys/filesys.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "threads/vaddr.h"
#include "userprog/pagedir.h"

#include "devices/shutdown.h"
#include "devices/input.h"
#include "userprog/process.h"

#include "vm/vm.h"
#include "vm/page.h"
#include "vm/page.h"

//Forward declarations
static void syscall_handler (struct intr_frame *);

//helper prototypes (only used in syscall.c)
static bool validate_range_user (const void *uaddr, size_t size);
static void copy_from_user (void *dst, const void *src_user, size_t size);
static int  fetch_arg_int (const void *user_esp, int index);
static void *fetch_arg_ptr(const void *user_esp, int index);
static void validate_string_user(const char *u_str);

bool is_valid_user_read (const void *uaddr, unsigned size);
bool is_valid_user_write (void *uaddr, unsigned int size);

static int fibonacci_kernel(int n);
static int max_of_four_int_kernel(int a, int b, int c, int d);

struct lock filesys_lock;

static void
check_user_buffer (const void *buffer, size_t size, bool writable) {
    if (buffer == NULL) return;
    
    struct thread *curr = thread_current();
    void *start = pg_round_down(buffer);
    void *end = pg_round_down((uint8_t *)buffer + size - 1);

    for (void *addr = start; addr <= end; addr += PGSIZE) {
        /* 1. 주소 유효성 검사 (기존 validate_range_user 활용 가능하면 활용) */
        if (!is_user_vaddr(addr)){
          curr->exit_status = -1;
          thread_exit();
        }
        struct page *p = spt_find_page(&curr->spt, addr);
        
        if (p != NULL && pagedir_get_page(curr->pagedir, addr) == NULL) {
            /* [수정] 로딩 실패 시 에러 처리 (exit -1) */
            if (!vm_do_claim_page(p)) {
                 curr->exit_status = -1;
                 thread_exit();
            }
        }
        
        /* 3. 쓰기 작업인데 페이지가 read-only면 에러 */
        if (writable && p != NULL && !p->writable) {
          curr->exit_status = -1;
          thread_exit();
        }
    }
}

void
syscall_init (void) 
{
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
  lock_init(&filesys_lock);
}

static void
syscall_handler (struct intr_frame *f) 
{
  thread_current ()->stack_pointer = f->esp;
  uintptr_t u_esp = (uintptr_t) f->esp;

  int sysno;

  if(!validate_range_user ((const void *)u_esp, 4))
    {
    thread_current()->exit_status = -1;
    thread_exit();
    }
  copy_from_user (&sysno, (const void *)u_esp, 4);

  switch(sysno)
    {
      case SYS_OPEN:
        { 
          const char *file_name = fetch_arg_ptr((const void *)f->esp, 0);
          validate_string_user(file_name);

          lock_acquire(&filesys_lock);
          struct file *file_obj = filesys_open(file_name);
          lock_release(&filesys_lock);

          if (file_obj == NULL) {
              f->eax = -1;
              break;
          }

          struct thread *t = thread_current();
          int fd;
          fd = -1;
          for (int i = 2; i < 128; i++) {
              if (t->file_descripor[i] == NULL) {
                  t->file_descripor[i] = file_obj;
                  fd = i;
                  break;
              }
          }

          // 5. 테이블이 꽉 찼을 경우, 열었던 파일을 다시 닫고 -1 반환
          if (fd == -1) {
              file_close(file_obj);
          }
          
          // 6. 성공적으로 할당된 fd 또는 실패(-1)를 반환
          f->eax = fd;
          break;
        }

      case SYS_EXIT:
        {
          int status;
          
          /* 1. 사용자 스택에 있는 첫 번째 인수(종료 상태 값)의 주소 유효성을 검증하고,
            안전하게 값을 가져옵니다. f->esp + 4는 첫 번째 인수를 가리킵니다. */
          if (!is_valid_user_read(f->esp + 4, sizeof(int))) 
          {
              // 유효하지 않은 메모리 주소를 인수로 넘긴 경우,
              // 악의적이거나 버그가 있는 프로세스로 간주하여 에러(-1)와 함께 강제 종료합니다.
              // 커널이 패닉하는 대신, 문제가 있는 프로세스만 안전하게 제거합니다.
              status = -1;
          } 
          else 
          {
              // 유효한 주소인 경우, 해당 주소에서 정수 값을 읽어옵니다.
              status = *(int *)(f->esp + 4);
              
              // 2. 실제 종료 로직은 process_exit 함수에게 위임합니다.
             
          }
          thread_current()->exit_status = status;
          thread_exit();
          /* process_exit는 절대 반환하지 않으므로, 이 break 문은 사실상 실행되지 않습니다.
            하지만 코드 구조의 일관성을 위해 유지합니다. */
          break;
        }

        case SYS_HALT:
        {
          shutdown_power_off();

          NOT_REACHED();
          break;
        }

        case SYS_EXEC:
        {
            const char *cmd_line_user = fetch_arg_ptr((const void *)f->esp, 0);
            validate_string_user(cmd_line_user); // 1. 유효성 검사

            // ★★★ 2. 커널 메모리로 즉시 복사 ★★★
            char *cmd_line_kernel = palloc_get_page(0);
            if (cmd_line_kernel == NULL) {
                f->eax = TID_ERROR;
                break;
            }
            strlcpy(cmd_line_kernel, cmd_line_user, PGSIZE);

            // 3. 커널 복사본으로 핵심 함수 호출
            f->eax = process_execute(cmd_line_kernel);

            // 4. 사용이 끝난 커널 복사본 해제
            palloc_free_page(cmd_line_kernel);
            break;
        }

        case SYS_WAIT:
        {
          // 1. 유저 스택에서 기다릴 자식의 tid를 가져온다.
          tid_t child_tid = fetch_arg_int((const void *)u_esp, 0);

          // 2. process_wait() 함수를 호출하여 실제 작업을 수행한다.
          int status = process_wait(child_tid);

          // 3. 반환된 값을 eax에 저장하여 유저 프로그램에 돌려준다.
          f->eax = status;
          break;
        }

        case SYS_WRITE:
        {
          int fd = fetch_arg_int((const void *)f->esp, 0);
          const void *buffer_user = fetch_arg_ptr((const void *)f->esp, 1);
          unsigned size = (unsigned)fetch_arg_int((const void *)f->esp, 2);

          if (!is_valid_user_read(buffer_user, size)) {
              thread_current()->exit_status = -1;
              thread_exit();
          }

          if (fd == 1) {
              char *buffer_kernel = palloc_get_page(0);
              if (buffer_kernel == NULL) {
                  f->eax = -1; 
                  break;
              }
              memcpy(buffer_kernel, buffer_user, size);

              putbuf(buffer_kernel, size);

              palloc_free_page(buffer_kernel);
              f->eax = size;
          } 
          else if(fd >= 2 && fd < 128){
            struct file *file_obj = thread_current()->file_descripor[fd];
            if (file_obj == NULL) {
                f->eax = -1;
            } else {
                // 👇 임계 구역 보호 시작
                check_user_buffer(buffer_user, size, false);
                lock_acquire(&filesys_lock);
                f->eax = file_write(file_obj, buffer_user, size);
                lock_release(&filesys_lock);
                // 👆 임계 구역 보호 끝
            }
          }
          else {
              f->eax = -1;
          }
          break;
        }

        case SYS_READ:
        {
          int fd = fetch_arg_int((const void *)f->esp, 0);
          void *buffer_user = fetch_arg_ptr((const void *)f->esp, 1);
          unsigned size = (unsigned)fetch_arg_int((const void *)f->esp, 2);

          if (!is_valid_user_write(buffer_user, size)) {
            thread_current()->exit_status = -1;
            thread_exit();
          }
          
          if (fd == 0) {
              uint8_t *buffer_kernel = palloc_get_page(0);
              if (buffer_kernel == NULL) {
                  f->eax = -1; // 메모리 부족
                  break;
              }
              
              unsigned i;
              for (i = 0; i < size; i++) {
                  buffer_kernel[i] = input_getc();
                  if (buffer_kernel[i] == '\n') {
                      i++; // '\n' 문자도 포함
                      break;
                  }
              }
              
              memcpy(buffer_user, buffer_kernel, i);
              palloc_free_page(buffer_kernel); // 사용 후 해제
              
              f->eax = i; // 실제로 읽은 바이트 수 반환
          } 
          else if (fd >= 2 && fd < 128){
            struct thread *t = thread_current();
            struct file *file_obj = t->file_descripor[fd];
            if (file_obj == NULL){
              f->eax = -1;
            }
            else{
              check_user_buffer(buffer_user, size, true);

              lock_acquire(&filesys_lock);
              f->eax = file_read(file_obj,buffer_user,size);
              lock_release(&filesys_lock);
            }
          }
          else {
              f->eax = -1;
          }
          break;
        }
        case SYS_REMOVE:
        {
          const char *file_name = fetch_arg_ptr((const void *)f->esp, 0);

          validate_string_user(file_name);

          lock_acquire(&filesys_lock);
          bool success = filesys_remove(file_name);
          lock_release(&filesys_lock);

          f->eax = success;
          break;
        }
        case SYS_FILESIZE:
        {
          int fd = fetch_arg_int((const void *)f->esp, 0);

          struct thread *t = thread_current();
          if (fd < 2 || fd >= 128 || t->file_descripor[fd] == NULL) {
              f->eax = -1; 
              break;
          }
          struct file *file_obj = t->file_descripor[fd];

          lock_acquire(&filesys_lock);
          off_t length = file_length(file_obj);
          lock_release(&filesys_lock);

          f->eax = length;
          break;
        }
        case SYS_SEEK:
        {
          int fd = fetch_arg_int((const void *)f->esp, 0);
          unsigned position = (unsigned)fetch_arg_int((const void *)f->esp, 1);

          struct thread *t = thread_current();
          if (fd < 2 || fd >= 128 || t->file_descripor[fd] == NULL) {
              break; // 반환 값 없을 경우..
          }
          struct file *file_obj = t->file_descripor[fd];

          lock_acquire(&filesys_lock);
          file_seek(file_obj, position);
          lock_release(&filesys_lock);

          break;
        }

        case SYS_TELL:
        {
          int fd = fetch_arg_int((const void *)f->esp, 0);

          struct thread *t = thread_current();
          if (fd < 2 || fd >= 128 || t->file_descripor[fd] == NULL) {
              f->eax = -1; // 유효하지 않은 fd
              break;
          }
          struct file *file_obj = t->file_descripor[fd];

          lock_acquire(&filesys_lock);
          unsigned position = file_tell(file_obj);
          lock_release(&filesys_lock);

          f->eax = position;
          break;
        }

        case SYS_CLOSE:
        {
          int fd = fetch_arg_int((const void *)f->esp, 0);

          struct thread *t = thread_current();
          if (fd < 2 || fd >= 128 || t->file_descripor[fd] == NULL) {
              break; // 유효하지 않은 fd - do nothing
          }
          struct file *file_obj = t->file_descripor[fd];

          lock_acquire(&filesys_lock);
          file_close(file_obj);
          lock_release(&filesys_lock);

          t->file_descripor[fd] = NULL;

          break;
        }
        
        case SYS_FIBONACCI:
        {
          // 사용자 스택에서 첫 번째 정수 인자 가져오기
          int n = fetch_arg_int((const void *)u_esp, 0);
          // 실제 피보나치 계산을 수행, 결과 eax 레지스터에 저장
          f->eax = fibonacci_kernel(n);
          break;
        }

        case SYS_MAX_OF_FOUR_INT:
        {
          int a = fetch_arg_int((const void *)f->esp, 0);
          int b = fetch_arg_int((const void *)f->esp, 1);
          int c = fetch_arg_int((const void *)f->esp, 2);
          int d = fetch_arg_int((const void *)f->esp, 3);

          f->eax = max_of_four_int_kernel(a, b, c, d); // 여기서 호출
          break;
        }

        case SYS_CREATE:
        {
          // 1. 사용자 스택에서 인자(파일 이름, 초기 크기)를 가져옵니다.
          const char *file_name = fetch_arg_ptr((const void *)f->esp, 0);
          unsigned initial_size = (unsigned)fetch_arg_int((const void *)f->esp, 1);

          // 2. 파일 이름 포인터와 문자열의 유효성을 검사합니다.
          validate_string_user(file_name);

          // 3. 파일 이름이 비어있는 경우, 생성에 실패 처리합니다.
          if (file_name == NULL || *file_name == '\0') {
              f->eax = false;
              break;
          }

          // 4. 동시성 문제를 막기 위해 파일 시스템 락을 획득합니다.
          lock_acquire(&filesys_lock);
          
          // 5. 실제 파일 생성 함수를 호출합니다.
          bool success = filesys_create(file_name, initial_size);
          
          // 6. 작업이 끝났으므로 락을 즉시 해제합니다.
          lock_release(&filesys_lock);

          // 7. 성공 여부(true 또는 false)를 eax 레지스터에 저장하여 반환합니다.
          f->eax = success;
          break;
        }


        /* userprog/syscall.c */

        case SYS_MMAP:
        {
            /* 1. Pintos 스펙에 맞춰 인자 파싱 (fd, addr) */
            int fd = fetch_arg_int((const void *)f->esp, 0);
            void *addr = fetch_arg_ptr((const void *)f->esp, 1);

            struct thread *curr = thread_current();
            struct file *file = NULL;

            /* 2. fd 검사 */
            if (fd >= 2 && fd < 128) {
                file = curr->file_descripor[fd];
            }
            
            /* 3. 유효성 검사 */
            if (file == NULL || addr == NULL) {
                f->eax = -1;
            } else {
                /* 4. 파일 크기 구하기 (length) */
                lock_acquire(&filesys_lock);
                int length = file_length(file);
                lock_release(&filesys_lock);

                if (length <= 0) {
                    f->eax = -1;
                } else {
                    /* 5. do_mmap 호출 (writable=true, offset=0 고정) */
                    void *mapped_addr = do_mmap(addr, length, true, file, 0);
                    
                    if (mapped_addr != NULL) {
                        /* 성공 시 mapid 반환 */
                        struct list_elem *e = list_back(&curr->mmap_list);
                        struct mmap_file *mf = list_entry(e, struct mmap_file, elem);
                        f->eax = mf->mapid;
                    } else {
                        f->eax = -1;
                    }
                }
            }
            break;
        }

        case SYS_MUNMAP:
        {
            int mapid = fetch_arg_int((const void *)f->esp, 0);
            struct thread *curr = thread_current();
            struct mmap_file *mmap_info = NULL;

            /* mapid로 해당 매핑 구조체 찾기 */
            struct list_elem *e;
            for (e = list_begin(&curr->mmap_list); e != list_end(&curr->mmap_list); e = list_next(e)) {
                struct mmap_file *temp = list_entry(e, struct mmap_file, elem);
                if (temp->mapid == mapid) {
                    mmap_info = temp;
                    break;
                }
            }

            /* 찾았으면 해제 수행 */
            if (mmap_info != NULL) {
                do_munmap(mmap_info);
            }
            break;
        }

              default:
                thread_current()->exit_status = -1;
                thread_exit();
                break;
            }
}

/*
  syscall_handler가 해야 하는 일 : 
    1. 커널에 진입할 때 cpu가 저장해둔 intr_frame의 포인터를 f로 받아온다.
    2. 이 포인터로 받아온 구조체 중 f->esp에 관심이 있다.
     esp : 시스템 콜이 트랩될 당시 유저 스택의 최상단 부분. 시스템 콜 번호가 놓이고 바로 위에 첫번째 인자가 놓인다. 첫번 째 인자에는 exit의 status가 기록된다.
    3. esp에 대해서는 다음 두 가지를 검사한다.
      3-1. esp가 유저 영역 내부에 있는가?
      3-2. esp + 0 ~ esp + 7 영역에서 이 위치의 memory 전체가 유효한가?
        esp + 0 ~ esp + 3 : 시스템 콜 번호
        esp + 4 ~ esp + 7 : exit(int status) 인자가 들어있음
    4. 검증을 완료했으면 시스템 콜 번호와 exit 인자를 가져온다.

*/

static bool
validate_range_user (const void *uaddr, size_t size)
{
  if (size == 0) return true;
  if (uaddr == NULL) return false;

  uintptr_t start = (uintptr_t)uaddr;
  uintptr_t end = start + size - 1;
  if (end < start) return false;

  if (!is_user_vaddr ((const void *)start)) return false;
  if (!is_user_vaddr ((const void *)end))   return false;


  // for (;;)
  //   {
  //     if (pagedir_get_page (thread_current ()->pagedir, (const void *)page) == NULL)
  //       return false;
  //     if (page == last) break;
  //     page += PGSIZE;
  //   }
  return true;
}

static void
copy_from_user (void *dst, const void *src_user, size_t size)
//size byte를 유저 공간(src_user)에서 커널 버퍼(dst)로 복사
{
  if (!validate_range_user (src_user, size)){
    thread_current()->exit_status = -1;
    thread_exit();
  }
  memcpy (dst, src_user, size);
}

static int
fetch_arg_int (const void *user_esp, int index)
{
  if (index < 0) {
    thread_current()->exit_status = -1;
    thread_exit();
  }

  const uint8_t *base = (const uint8_t *) user_esp; 
  size_t off = 4u * (size_t)(index + 1);
  const void *arg_addr = (const void *)(base + off); //시스템 콜 스택에서 index 번째 인자를 갖고 온다.
  
  int val = 0; 
  copy_from_user (&val, arg_addr, sizeof val);
  //유저 메모리 arg_addr에서 4바이트를 커널 버퍼 val로 복사
  return val;
}

static void *
fetch_arg_ptr (const void *user_esp, int index)
{
  if (index < 0){
    thread_current()->exit_status = -1;
    thread_exit();
  }
  // 시스템 콜 번호는 인자가 아니므로 index+1
  const void *arg_addr = (const uint8_t *)user_esp + (4 * (index + 1));
  
  void *ptr = NULL;
  // user_esp에 있는 포인터 값 자체를 커널로 복사
  copy_from_user(&ptr, arg_addr, sizeof(ptr));
  
  return ptr;
}

// u_str 포인터와 포인터가 가리키는 문자열 전체의 유효성을 검증하는 함수
static void
validate_string_user(const char *u_str)
{
  // 1. 포인터 자체의 주소가 유효한지, NULL은 아닌지 먼저 확인
  if (!validate_range_user(u_str, 1)){
    thread_current()->exit_status = -1;
    thread_exit();
  }
  // 2. 문자열의 끝(NULL)을 만날 때까지 한 글자씩 따라가며 주소 유효성 검사
  const char *p = u_str;
  while (true) {
    if (!validate_range_user(p, 1)){
      thread_current()->exit_status = -1;
      thread_exit();
    }
    if (*p == '\0')
      break;
    p++;
  }
}

// is_valid_user_read/write 함수 구현
bool
is_valid_user_read (const void *uaddr, unsigned size)
{
    return validate_range_user(uaddr, size);
}

bool
is_valid_user_write (void *uaddr, unsigned size)
{
    return validate_range_user(uaddr, size);
}


//2 additional system call function

static int
fibonacci_kernel(int n)
{
  if (n < 0) return -1;
  if (n == 0) return 0;
  if (n == 1) return 1;

  int a = 0, b = 1, c;
  for (int i = 2; i <= n; i++) {
      c = a + b;
      a = b;
      b = c;
  }
  return b;
}

static int 
max_of_four_int_kernel(int a, int b, int c, int d)
{
  int max = a;
  if (b > max) max = b;
  if (c > max) max = c;
  if (d > max) max = d;
  return max;
}


