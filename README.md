# Pintos Operating System

> **A comprehensive implementation of a monolithic kernel operating system for the x86 architecture.** > Developed as part of the Operating Systems curriculum, focusing on concurrency, memory virtualization, and file system architecture.

![Language](https://img.shields.io/badge/language-C-blue.svg) ![Assembly](https://img.shields.io/badge/arch-x86_Assembly-red.svg) ![Platform](https://img.shields.io/badge/platform-Linux%20%7C%20QEMU-lightgrey.svg) ![Completion](https://img.shields.io/badge/status-All_Tests_Passed-success.svg)

## Project Abstract
This project involves building key components of the **Pintos** operating system, a simple yet functional OS framework for the 80x86 architecture. The development process covered the entire lifecycle of an OS, from the kernel bootstrap to user process management, virtual memory virtualization, and file system implementation.

The primary goal was to understand the underlying principles of modern operating systems by implementing them from scratch. This repository demonstrates a robust kernel capable of running user programs with strict memory isolation, efficient scheduling, and a reliable file system.

## Key Features & Achievements

### **1. Advanced Scheduling (Threads)**
* **Priority Scheduling:** Implemented strict priority scheduling with **Priority Donation** to solve the priority inversion problem.
* [cite_start]**MLFQS (Multi-Level Feedback Queue Scheduler):** Developed a 4.4BSD-style dynamic scheduler using **17.14 Fixed-Point Arithmetic** to handle system load and CPU usage calculations without floating-point hardware support[cite: 15, 90].
* [cite_start]**Efficient Sleeping:** Replaced busy-waiting with a `sleep_list` mechanism to maximize CPU utilization[cite: 8].

### **2. User Program Execution**
* [cite_start]**System Call Interface:** Built a secure handler for essential system calls (`exec`, `wait`, `read`, `write`, etc.), enabling user interaction with the kernel[cite: 310].
* **Argument Passing:** Implemented x86 calling convention compliant argument passing for process creation.
* **Process Hierarchy:** Managed parent-child relationships and exit status propagation to prevent zombie processes.

### **3. Virtual Memory Management**
* [cite_start]**Demand Paging (Lazy Loading):** Implemented a loading mechanism that allocates physical memory only when a page is accessed, reducing initialization overhead[cite: 311, 436].
* [cite_start]**Supplemental Page Table:** Designed a **Hash Table-based** page management system for O(1) access performance[cite: 397].
* [cite_start]**Swap System:** Developed a swap mechanism using a bitmap to manage disk slots, enabling the system to handle workloads exceeding physical memory size[cite: 304, 451].
* [cite_start]**Stack Growth:** Supported dynamic stack expansion with heuristic validity checks to handle recursion and large local allocations[cite: 306, 346].

### **4. Robustness & Concurrency Control**
* [cite_start]**Synchronization:** Addressed critical race conditions and deadlocks (e.g., in `sema_up` and file system locks) using fine-grained locking and ordering enforcement[cite: 137, 507].
* [cite_start]**Transaction-based Memory Loading:** Implemented a "Load-Map-Commit" model to prevent data loss during page faults and swap operations[cite: 500].
* **Secure Memory Access:** Enforced strict user-kernel memory isolation to prevent kernel panics from malicious or malformed user pointers.

---

# Project 1: User Programs

## Overview
The primary goal of this project was to extend the basic Pintos kernel to support the execution of user programs. This involved setting up the user memory space, implementing argument passing, and establishing a secure system call interface to handle interactions between user processes and the kernel.

## Key Implementations

### 1. Argument Passing
* **Command Line Parsing:** Implemented logic to tokenize command-line inputs, separating the executable name from its arguments.
* **Stack Setup:** Constructed the user stack adhering to the **x86 calling convention**.
    * Pushed arguments in reverse order.
    * Correctly set up `argv` pointers and `argc`.
    * Ensured 16-byte stack alignment for stable execution.

### 2. System Call Infrastructure
* **Handler Implementation:** Developed a robust `syscall_handler` in `userprog/syscall.c` to intercept interrupts (`INT 0x30`) and dispatch requests.
* **Process Management:**
    * **`exec`**: Validates the file and creates a child process.
    * **`wait`**: Implemented synchronization logic to allow the parent process to wait for the child's termination and retrieve its exit status correctly.
    * **`exit`**: Ensures clean process termination and resource deallocation.
* **File I/O:** Implemented standard file operations (`open`, `read`, `write`, `close`, etc.) with strict validation to protect the file system.

### 3. Extended System Calls (Custom Features)
To deepen the understanding of the system call mechanism and stack manipulation, I implemented custom system calls beyond the standard requirements:

* **Custom Functions:**
    * `int fibonacci(int n)`: Computes the n-th Fibonacci number in kernel space.
    * `int max_of_four_int(int a, int b, int c, int d)`: Calculates the maximum value among four integers.
* **Macro Expansion (`syscall4`):**
    * Since the default Pintos distribution only supported up to 3 arguments, I implemented a **`syscall4` macro** in `lib/user/syscall.c`.
    * Modified the inline assembly to handle the additional argument push and correctly adjusted the stack pointer (`addl $20, %%esp`) after the call.
* **Safe Argument Retrieval:**
    * Implemented `fetch_arg_int()` to safely retrieve arguments from the user stack, ensuring that the kernel reads from valid user memory addresses only.

### 4. Memory Safety & Isolation
* **Address Validation:** Implemented strict address checking (`check_address`) to prevent user programs from accessing kernel space or unmapped memory.
* **Fault Handling:** Ensured that invalid memory accesses result in the offending process being terminated (`exit(-1)`) rather than causing a kernel panic, thereby maintaining OS stability.

# Project 2: User Programs

## Overview
The objective of this project was to establish a system call interface enabling user programs to interact with the kernel safely. The focus was on implementing process lifecycle management, file input/output operations, and ensuring strict isolation between user and kernel space.

## Key Implementations

### 1. File Descriptor Table
* **Structure:** Implemented a per-process file descriptor table using an array of pointers (`struct file *file_descriptor_table[128]`) within the `thread` structure.
* **O(1) Access:** Used integer file descriptors (fd) as direct indices into the array, ensuring **O(1)** access time for file operations, which is significantly faster than linked list or tree-based approaches.
* **Standard I/O Management:**
    * Reserved `fd 0` for Standard Input (STDIN) using `input_getc()`.
    * Reserved `fd 1` for Standard Output (STDOUT) using `putbuf()`.
* **Resource Cleanup:** Enforced automatic closure of all open file descriptors upon process termination (`process_exit`) to prevent resource leaks.

### 2. System Call Infrastructure
Implemented a robust handler for the `INT 0x30` interrupt to manage system calls.

* **Process Management:**
    * **`exec`**: Implemented `process_execute` to parse command-line arguments and create child processes.
    * **`wait`**: Designed `process_wait` using semaphores (`wait_sema`) to synchronize parent-child execution and retrieve exit status.
    * **`exit`**: Managed process termination, updating the `child_status` structure to notify the parent and cleaning up resources.
* **File Operations:**
    * Implemented `create`, `remove`, `open`, `filesize`, `read`, `write`, `seek`, `tell`, and `close`.
    * **Executable Protection:** Added logic to deny write access to currently running executables (`file_deny_write`) to ensure system stability during execution.

### 3. Synchronization in Filesystem
* **Coarse-Grained Locking:** Adopted a global lock strategy (`filesys_lock`) to ensure mutual exclusion for all file system operations.
    * This atomicity prevents race conditions where multiple processes might simultaneously modify the free map or directory structure.
    * **Critical Sections:** Applied `lock_acquire` and `lock_release` around all file system calls (`filesys_*`, `file_*`) in `syscall_handler` and resource cleanup routines.
* **Signaling:** Utilized Semaphores (`load_sema`, `wait_sema`) strictly for process signaling (execution order control) rather than mutual exclusion, maintaining a clear separation of concerns.

## Challenges & Troubleshooting

### Multi-oom (Out of Memory) & Resource Leaks
* **Issue:** The `multi-oom` test failed repeatedly due to memory leaks when `thread_create` or `load` failed.
* **Solution:**
    * Implemented comprehensive cleanup logic in `process_execute` to free `child_status`, arguments, and parsed strings upon failure.
    * Fixed a **Use-After-Free** bug by decoupling the child from the parent (`as_child_in_parent = NULL`) before the child exits on load failure.

### Synchronization Failures
* **Issue:** `syn-read` and `syn-write` tests failed due to race conditions accessing shared file data.
* **Solution:** Systematically applied the global `filesys_lock` to all file system entry points, ensuring that operations like file creation and writing are atomic.

### Invariant Violation (Panic)
* **Issue:** A kernel panic occurred due to `inode->deny_write_cnt` mismatch during process exit.
* **Analysis:** `file_close()` was being called before `file_allow_write()`, violating the inode's internal state.
* **Solution:** Adjusted the order in `process_exit` to call `file_allow_write()` *before* `file_close()` and protected the entire sequence with `filesys_lock`.

# Project 3: Threads

## Overview
The goal of this project was to improve the basic round-robin scheduler of the Pintos kernel by implementing efficient thread sleeping mechanisms and advanced scheduling algorithms. I replaced busy-waiting with an alarm clock system and implemented both strict priority scheduling and a Multi-Level Feedback Queue Scheduler (MLFQS) to ensure fairness and system responsiveness.

## Key Implementations

### 1. Alarm Clock
* [cite_start]**Elimination of Busy-Waiting:** Replaced the inefficient `timer_sleep()` implementation, which wasted CPU cycles in a loop, with a blocking mechanism[cite: 7, 23].
* **Sleep List Management:**
    * [cite_start]Implemented `thread_sleep()` to switch threads to the `BLOCKED` state and insert them into a global `sleep_list` ordered by their wakeup time[cite: 8, 25].
    * [cite_start]**Wake-up Mechanism:** Designed `thread_wake_up()` to be called every tick; it checks the `sleep_list` and immediately moves threads whose wakeup time has arrived to the `READY` state[cite: 9, 27].
    * [cite_start]This significantly improved system resource utilization by allowing the CPU to execute other tasks while threads are waiting[cite: 10, 26].

### 2. Priority Scheduling
* [cite_start]**Strict Priority Enforcement:** Modified the scheduler to ensure that the highest priority thread in the `ready_list` always gets the CPU[cite: 34].
* **Ordered Lists:**
    * [cite_start]Implemented `thread_priority_less` comparator and updated `list_insert_ordered()` usage for both the `ready_list` and semaphore `waiters` list to keep them sorted by priority[cite: 12, 128].
    * [cite_start]Ensured **LIFO (Last-In-First-Out)** behavior for threads with equal priority by using the `>=` operator in comparison[cite: 127, 218].
* **Preemption Logic:**
    * [cite_start]Implemented preemption in `thread_create`, `thread_unblock`, and `thread_set_priority`[cite: 13, 130].
    * [cite_start]If a newly ready thread has a higher priority than the currently running thread, `thread_yield()` (or `intr_yield_on_return`) is called immediately to yield the CPU[cite: 63, 64].
* [cite_start]**Priority Aging:** Implemented `thread_aging()` to increment the priority of waiting threads periodically, preventing starvation of low-priority tasks[cite: 134, 163].

### 3. Advanced Scheduler (MLFQS)
* [cite_start]**Dynamic Priority Adjustment:** Implemented a 4.4BSD-style scheduler where priority is dynamically recalculated based on `recent_cpu` usage and system `load_avg`[cite: 15, 46].
    * [cite_start]**Formula:** `priority = PRI_MAX - (recent_cpu / 4) - (nice * 2)`[cite: 46].
    * [cite_start]**Decay Mechanism:** Implemented a decay formula for `recent_cpu` so that I/O-bound threads (which sleep often) gain higher priority for quick responsiveness[cite: 48, 49].
* **Fixed-Point Arithmetic:**
    * [cite_start]Since the Pintos kernel does not support floating-point operations, I implemented a **17.14 fixed-point arithmetic** library (`fixedpointarith.h`)[cite: 16, 90].
    * [cite_start]This module handles complex calculations for `load_avg` and `recent_cpu` with high precision, avoiding overflow[cite: 247, 249].

## Challenges & Troubleshooting

### Synchronization & Deadlock in `sema_up`
* **Issue:** A race condition occurred in the `priority-sema` test. When `sema_up` called `thread_unblock`, a higher-priority thread woke up and preempted the current thread immediately. [cite_start]However, since `sema->value` hadn't been incremented yet, the woken thread tried to `sema_down`, saw `value == 0`, and went back to sleep, causing a deadlock[cite: 138, 169].
* **Solution:** Modified the order of operations in `sema_up` within the critical section (interrupts disabled). [cite_start]I ensured `sema->value++` is executed *before* calling `thread_unblock()`, guaranteeing that the woken thread sees the correct semaphore value[cite: 137, 170].

### MLFQS Integration Conflicts
* [cite_start]**Issue:** Compile errors occurred because the Priority Donation logic (from the static priority scheduler) referenced fields that were unnecessary for MLFQS[cite: 277].
* [cite_start]**Solution:** completely removed Priority Donation logic from `lock_acquire` and `lock_release` when running in MLFQS mode to resolve conflicts and ensure stability[cite: 279].

# Project 3: Threads

## Overview
The goal of this project was to improve the basic round-robin scheduler of the Pintos kernel by implementing efficient thread sleeping mechanisms and advanced scheduling algorithms. I replaced busy-waiting with an alarm clock system and implemented both strict priority scheduling and a Multi-Level Feedback Queue Scheduler (MLFQS) to ensure fairness and system responsiveness.

## Key Implementations

### 1. Alarm Clock
* [cite_start]**Elimination of Busy-Waiting:** Replaced the inefficient `timer_sleep()` implementation, which wasted CPU cycles in a loop, with a blocking mechanism[cite: 7, 23].
* **Sleep List Management:**
    * [cite_start]Implemented `thread_sleep()` to switch threads to the `BLOCKED` state and insert them into a global `sleep_list` ordered by their wakeup time[cite: 8, 25].
    * [cite_start]**Wake-up Mechanism:** Designed `thread_wake_up()` to be called every tick; it checks the `sleep_list` and immediately moves threads whose wakeup time has arrived to the `READY` state[cite: 9, 27].
    * [cite_start]This significantly improved system resource utilization by allowing the CPU to execute other tasks while threads are waiting[cite: 10, 26].

### 2. Priority Scheduling
* [cite_start]**Strict Priority Enforcement:** Modified the scheduler to ensure that the highest priority thread in the `ready_list` always gets the CPU[cite: 34].
* **Ordered Lists:**
    * [cite_start]Implemented `thread_priority_less` comparator and updated `list_insert_ordered()` usage for both the `ready_list` and semaphore `waiters` list to keep them sorted by priority[cite: 12, 128].
    * [cite_start]Ensured **LIFO (Last-In-First-Out)** behavior for threads with equal priority by using the `>=` operator in comparison[cite: 127, 218].
* **Preemption Logic:**
    * [cite_start]Implemented preemption in `thread_create`, `thread_unblock`, and `thread_set_priority`[cite: 13, 130].
    * [cite_start]If a newly ready thread has a higher priority than the currently running thread, `thread_yield()` (or `intr_yield_on_return`) is called immediately to yield the CPU[cite: 63, 64].
* [cite_start]**Priority Aging:** Implemented `thread_aging()` to increment the priority of waiting threads periodically, preventing starvation of low-priority tasks[cite: 134, 163].

### 3. Advanced Scheduler (MLFQS)
* [cite_start]**Dynamic Priority Adjustment:** Implemented a 4.4BSD-style scheduler where priority is dynamically recalculated based on `recent_cpu` usage and system `load_avg`[cite: 15, 46].
    * [cite_start]**Formula:** `priority = PRI_MAX - (recent_cpu / 4) - (nice * 2)`[cite: 46].
    * [cite_start]**Decay Mechanism:** Implemented a decay formula for `recent_cpu` so that I/O-bound threads (which sleep often) gain higher priority for quick responsiveness[cite: 48, 49].
* **Fixed-Point Arithmetic:**
    * [cite_start]Since the Pintos kernel does not support floating-point operations, I implemented a **17.14 fixed-point arithmetic** library (`fixedpointarith.h`)[cite: 16, 90].
    * [cite_start]This module handles complex calculations for `load_avg` and `recent_cpu` with high precision, avoiding overflow[cite: 247, 249].

## Challenges & Troubleshooting

### Synchronization & Deadlock in `sema_up`
* **Issue:** A race condition occurred in the `priority-sema` test. When `sema_up` called `thread_unblock`, a higher-priority thread woke up and preempted the current thread immediately. [cite_start]However, since `sema->value` hadn't been incremented yet, the woken thread tried to `sema_down`, saw `value == 0`, and went back to sleep, causing a deadlock[cite: 138, 169].
* **Solution:** Modified the order of operations in `sema_up` within the critical section (interrupts disabled). [cite_start]I ensured `sema->value++` is executed *before* calling `thread_unblock()`, guaranteeing that the woken thread sees the correct semaphore value[cite: 137, 170].

### MLFQS Integration Conflicts
* [cite_start]**Issue:** Compile errors occurred because the Priority Donation logic (from the static priority scheduler) referenced fields that were unnecessary for MLFQS[cite: 277].
* [cite_start]**Solution:** completely removed Priority Donation logic from `lock_acquire` and `lock_release` when running in MLFQS mode to resolve conflicts and ensure stability[cite: 279].


# Project 4: Virtual Memory

## Overview
The primary objective of this project was to overcome the limitations of physical memory by implementing a robust virtual memory system. I developed a Supplemental Page Table (SPT) to manage page states, implemented demand paging (Lazy Loading), designed a swap system to handle memory pressure, and supported dynamic stack growth and memory-mapped files (`mmap`).

## Key Implementations

### 1. Supplemental Page Table (SPT)
* **Data Structure:** Adopted a **Hash Table** (`lib/kernel/hash.c`) to manage the supplemental page table for each process, ensuring **O(1)** access time for page lookups compared to O(N) linked lists.
* **Page Types:** Designed `struct page` using a **union** to efficiently handle different page types (`VM_UNINIT`, `VM_ANON`, `VM_FILE`) sharing the same memory space.
* **Lazy Loading:** Implemented `vm_alloc_page_with_initializer` to delay physical memory allocation until the page is actually accessed (Page Fault), significantly reducing initialization overhead.

### 2. Page Fault Handling & Stack Growth
* **Fault Handler:** Implemented `vm_try_handle_fault` to validate addresses and delegate processing.
    * Checks if the address is valid (user space, not NULL).
    * Determines whether to claim an existing page or grow the stack.
* **Stack Growth:**
    * Implemented a heuristic to detect valid stack expansion requests: `addr >= rsp - 32`.
    * Supported dynamic stack allocation up to a **1MB limit**, allocating new anonymous pages (`VM_ANON`) as needed.

### 3. Frame Management & Swap System
* **Clock Algorithm:** Implemented the **Clock (Second Chance) Algorithm** for page replacement. It iterates through the frame table, checking the hardware `accessed bit` to approximate LRU (Least Recently Used) behavior with low overhead.
* **Swap System:**
    * Managed swap slots using a **Bitmap**, where each bit represents 8 consecutive sectors (4KB).
    * Decoupled swap initialization (`swap_init`) from file system initialization to prevent dependency issues.
    * Ensured data integrity by separating `swap_in` (load) and `swap_free` (release) logic.

### 4. Memory Mapped Files (mmap)
* **`mmap` System Call:** Implemented lazy loading for file-backed pages. A dedicated `file_reopen` ensures the file handle remains valid even if the user closes the original file descriptor.
* **`munmap` & Write-back:**
    * Utilized the hardware `dirty bit` to minimize disk I/O.
    * Only pages modified since loading are written back to the disk (`file_write_at`) upon unmapping or process exit.

## Challenges & Troubleshooting

### Concurrency Control: Frame Theft
* **Issue:** In a multi-threaded environment, a frame allocated to Thread A could be evicted by Thread B before Thread A finished loading data, causing data corruption ("bad value 0").
* **Solution (Delayed Linking):**
    * Initially set `frame->page = NULL` upon allocation.
    * The eviction algorithm skips frames with `NULL` page pointers (treating them as "under initialization").
    * Linked `frame->page = page` only after data loading completes.

### Transactional Page Claim
* **Issue:** If page table mapping failed after swapping in data, the swap slot was already freed, leading to permanent data loss.
* **Solution:** Implemented a **Load-Map-Commit** transaction model.
    1.  **Load:** Read data from swap disk.
    2.  **Map:** Attempt to set up the page table (`pagedir_set_page`).
    3.  **Commit:** Call `swap_free` to release the swap slot *only if* mapping succeeds.

### Deadlock (Recursive Locking)
* **Issue:** Accessing a user buffer inside a system call (holding `filesys_lock`) caused a page fault, which tried to acquire `filesys_lock` again to load the page, resulting in a deadlock.
* **Solution (Pre-loading):** Implemented `check_user_buffer` in `syscall.c`. Before acquiring the lock, the kernel inspects the user buffer and pre-loads (`vm_claim_page`) any missing pages to prevent faults within the critical section.

### Executable Write Protection
* **Issue:** Executable files are read-only while running. If their data segments (e.g., global variables) were modified and then evicted, they couldn't be written back to the file, causing data loss.
* **Solution:** Implemented **Type Conversion**. Upon loading a data segment from an executable, its type is immediately converted from `VM_FILE` to `VM_ANON`. This forces the system to write modified data to the **Swap Disk** instead of the original file during eviction.
