diff --git a/Makefile b/Makefile
index af1e7a5..6711ba0 100644
--- a/Makefile
+++ b/Makefile
@@ -150,6 +150,7 @@ UPROGS=\
 	$U/_forphan\
 	$U/_dorphan\
 	$U/_sync\
+	$U/_test_scheduler\
 
 fs.img: mkfs/mkfs README $(UPROGS)
 	mkfs/mkfs fs.img README $(UPROGS)
@@ -171,7 +172,7 @@ QEMUGDB = $(shell if $(QEMU) -help | grep -q '^-gdb'; \
 	then echo "-gdb tcp::$(GDBPORT)"; \
 	else echo "-s -p $(GDBPORT)"; fi)
 ifndef CPUS
-CPUS := 3
+CPUS := 1
 endif
 
 QEMUOPTS = -machine virt -bios none -kernel $K/kernel -m 128M -smp $(CPUS) -nographic
diff --git a/kernel/proc.c b/kernel/proc.c
index f4c1668..183f963 100644
--- a/kernel/proc.c
+++ b/kernel/proc.c
@@ -20,6 +20,34 @@ static void freeproc(struct proc *p);
 
 extern char trampoline[]; // trampoline.S
 
+// --- Lottery scheduling: pseudorandom number generator ---
+// Simple, deterministic PRNG so we don't depend on any hardware
+// randomness source (xv6-riscv doesn't provide one).
+int
+do_rand(unsigned long *ctx)
+{
+  long hi, lo, x;
+
+  // Transform to [1, 0x7ffffffe] range.
+  x = (*ctx % 0x7ffffffe) + 1;
+  hi = x / 127773;
+  lo = x % 127773;
+  x = 16807 * lo - 2836 * hi;
+  if (x < 0)
+    x += 0x7fffffff;
+  x--;
+  *ctx = x;
+  return (x);
+}
+
+unsigned long rand_next = 1;
+
+int
+rand(void)
+{
+  return (do_rand(&rand_next));
+}
+
 // helps ensure that wakeups of wait()ing
 // parents are not lost. helps obey the
 // memory model when using p->parent.
@@ -146,6 +174,12 @@ found:
   p->context.ra = (uint64)forkret;
   p->context.sp = p->kstack + PGSIZE;
 
+  // Lottery scheduling: every new process starts with 1 ticket and
+  // 0 rounds run. userinit() bumps init's ticket count to 10, and
+  // kfork() makes children inherit their parent's ticket count.
+  p->tickets = 1;
+  p->rounds = 0;
+
   return p;
 }
 
@@ -168,6 +202,8 @@ freeproc(struct proc *p)
   p->killed = 0;
   p->xstate = 0;
   p->state = UNUSED;
+  p->tickets = 0;
+  p->rounds = 0;
 }
 
 // Create a user page table for a given process, with no user memory,
@@ -225,6 +261,9 @@ userinit(void)
 
   p->cwd = namei("/");
 
+  // Task 1: the initial process (init) starts with 10 tickets.
+  p->tickets = 10;
+
   p->state = RUNNABLE;
 
   release(&p->lock);
@@ -275,6 +314,9 @@ kfork(void)
   }
   np->sz = p->sz;
 
+  // Task 1: a forked child inherits its parent's ticket count.
+  np->tickets = p->tickets;
+
   // copy saved user registers.
   *(np->trapframe) = *(p->trapframe);
 
@@ -441,31 +483,56 @@ scheduler(void)
     intr_on();
     intr_off();
 
+    // --- Lottery scheduling (Task 4) ---
+
+    // 1) Add up the tickets held by every RUNNABLE process.
+    int total_tickets = 0;
+    for (p = proc; p < &proc[NPROC]; p++) {
+      acquire(&p->lock);
+      if (p->state == RUNNABLE)
+        total_tickets += p->tickets;
+      release(&p->lock);
+    }
+
+    if (total_tickets <= 0) {
+      // Nobody is runnable; stop running on this core until an interrupt.
+      asm volatile("wfi");
+      continue;
+    }
+
+    // 2) Draw the winning ticket number in [0, total_tickets - 1].
+    int winner = rand() % total_tickets;
+
+    // 3) Walk the table again, adding up tickets as we go, until the
+    //    running sum passes the winning number: that process holds
+    //    the winning ticket.
+    int counted = 0;
     int found = 0;
     for (p = proc; p < &proc[NPROC]; p++) {
       acquire(&p->lock);
       if (p->state == RUNNABLE) {
-        // Switch to chosen process.  It is the process's job
-        // to release its lock and then reacquire it
-        // before jumping back to us.
-        p->state = RUNNING;
-        c->proc = p;
-        swtch(&c->context, &p->context);
-
-        // Don't re-enable interrupts on release.
-        mycpu()->intena = 0;
-
-        // Process is done running for now.
-        // It should have changed its p->state before coming back.
-        c->proc = 0;
-        found = 1;
+        counted += p->tickets;
+        if (!found && counted > winner) {
+          found = 1;
+
+          // Switch to the winning process. It is the process's job
+          // to release its lock and then reacquire it
+          // before jumping back to us.
+          p->state = RUNNING;
+          p->rounds++; // count this scheduling round for the process
+          c->proc = p;
+          swtch(&c->context, &p->context);
+
+          // Don't re-enable interrupts on release.
+          mycpu()->intena = 0;
+
+          // Process is done running for now.
+          // It should have changed its p->state before coming back.
+          c->proc = 0;
+        }
       }
       release(&p->lock);
     }
-    if (found == 0) {
-      // nothing to run; stop running on this core until an interrupt.
-      asm volatile("wfi");
-    }
   }
 }
 
@@ -695,7 +762,8 @@ procdump(void)
       state = states[p->state];
     else
       state = "???";
-    printk("%d %s %s", p->pid, state, p->name);
+    printk("%d %s %s tickets=%d rounds=%d", p->pid, state, p->name,
+           p->tickets, p->rounds);
     printk("\n");
   }
 }
diff --git a/kernel/proc.h b/kernel/proc.h
index 5603697..b164abd 100644
--- a/kernel/proc.h
+++ b/kernel/proc.h
@@ -101,4 +101,9 @@ struct proc {
   struct file *ofile[NOFILE];  // Open files
   struct inode *cwd;           // Current directory
   char name[16];               // Process name (debugging)
+
+  // Lottery scheduling fields (private to the process, p->lock need not be held
+  // for reads that happen to race, but we always take p->lock in practice).
+  int tickets;                 // Number of lottery tickets this process holds
+  int rounds;                  // Number of times the scheduler has picked this process
 };
diff --git a/kernel/syscall.c b/kernel/syscall.c
index 1800f20..47bf648 100644
--- a/kernel/syscall.c
+++ b/kernel/syscall.c
@@ -103,6 +103,7 @@ extern uint64 sys_link(void);
 extern uint64 sys_mkdir(void);
 extern uint64 sys_close(void);
 extern uint64 sys_sync(void);
+extern uint64 sys_settickets(void);
 
 // An array mapping syscall numbers from syscall.h
 // to the function that handles the system call.
@@ -130,6 +131,7 @@ static uint64 (*syscalls[])(void) = {
   [SYS_mkdir]   = sys_mkdir,
   [SYS_close]   = sys_close,
   [SYS_sync]    = sys_sync,
+  [SYS_settickets] = sys_settickets,
   // clang-format on
 };
 
diff --git a/kernel/syscall.h b/kernel/syscall.h
index 5a4a39f..948497a 100644
--- a/kernel/syscall.h
+++ b/kernel/syscall.h
@@ -21,3 +21,4 @@
 #define SYS_mkdir  20
 #define SYS_close  21
 #define SYS_sync   22
+#define SYS_settickets 23
diff --git a/kernel/sysproc.c b/kernel/sysproc.c
index cd25557..5dbd73b 100644
--- a/kernel/sysproc.c
+++ b/kernel/sysproc.c
@@ -98,6 +98,21 @@ sys_kill(void)
   return kkill(pid);
 }
 
+// Task 2: let a process set its own number of lottery tickets.
+// n must be a positive integer; returns -1 on invalid input.
+uint64
+sys_settickets(void)
+{
+  int n;
+
+  argint(0, &n);
+  if (n <= 0)
+    return -1;
+
+  myproc()->tickets = n;
+  return 0;
+}
+
 // return how many clock tick interrupts have occurred
 // since start.
 uint64
diff --git a/user/test_scheduler.c b/user/test_scheduler.c
new file mode 100644
index 0000000..d9183ab
--- /dev/null
+++ b/user/test_scheduler.c
@@ -0,0 +1,50 @@
+// Task 6: a tiny user program used to test the lottery scheduler.
+//
+// Usage:
+//   test_scheduler <tickets>
+//
+// It sets its own ticket count using settickets(), then spins
+// forever so it stays RUNNABLE/RUNNING and keeps competing for the
+// CPU. Run a few copies in the background with different ticket
+// counts and watch the "rounds" column in the Ctrl+P (procdump)
+// output: over time each process's rounds should grow roughly in
+// proportion to its ticket count.
+//
+//   $ test_scheduler 10 &
+//   $ test_scheduler 5 &
+//   $ test_scheduler 2 &
+//   (press Ctrl+P a few times, a little while apart, to watch it)
+
+#include "kernel/types.h"
+#include "kernel/stat.h"
+#include "user/user.h"
+
+int
+main(int argc, char *argv[])
+{
+  int tickets;
+
+  if (argc != 2) {
+    printf("Usage: %s <tickets>\n", argv[0]);
+    exit(1);
+  }
+
+  tickets = atoi(argv[1]);
+  if (tickets <= 0) {
+    printf("%s: tickets must be a positive integer\n", argv[0]);
+    exit(1);
+  }
+
+  if (settickets(tickets) < 0) {
+    printf("%s: settickets failed\n", argv[0]);
+    exit(1);
+  }
+
+  // Spin forever so the process stays runnable and keeps
+  // participating in the lottery.
+  for (;;) {
+    ;
+  }
+
+  exit(0);
+}
diff --git a/user/user.h b/user/user.h
index 29aad59..f8623db 100644
--- a/user/user.h
+++ b/user/user.h
@@ -25,6 +25,7 @@ char *sys_sbrk(int, int);
 int pause(int);
 int uptime(void);
 int sync(void);
+int settickets(int);
 
 // ulib.c
 int stat(const char *, struct stat *);
diff --git a/user/usys.pl b/user/usys.pl
index 4d054e8..59d43b2 100755
--- a/user/usys.pl
+++ b/user/usys.pl
@@ -43,3 +43,4 @@ entry("sbrk");
 entry("pause");
 entry("uptime");
 entry("sync");
+entry("settickets");