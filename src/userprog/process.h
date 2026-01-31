#ifndef USERPROG_PROCESS_H
#define USERPROG_PROCESS_H

#include "threads/thread.h"
#include "threads/synch.h"

struct child_status{
  tid_t tid;                 /* 자식 TID 식별용 */
  int exit_code;             /* 자식의 exit(status) */
  bool exited;               /* 자식 종료 여부 */
  bool waited;               /* 부모가 이미 wait로 회수했는지 */
  bool is_orphan;            /* 고아 상태 여부 확인 (새롭게 추가)*/
  struct semaphore wait_sema;/* 부모가 기다릴 세마포어 */
  struct list_elem elem;     /* 부모 children 리스트에 들어갈 링크 */
  struct semaphore load_sema;
  bool load_ok;

  bool is_alive;
};

struct start_args{
  char *cmdline;
  struct child_status *cs;
};
//자식 스레드에 인수를 전달할 구조체이다.
//cmdline은 실행할 명령어 라인이고,
//cs는 부모와 공유할 자식의 상태 구조체 포인터이다.


tid_t process_execute (const char *file_name);
int process_wait (tid_t);
void process_exit (void);
void process_activate (void);

#endif /* userprog/process.h */
