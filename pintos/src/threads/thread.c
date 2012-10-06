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
#ifdef USERPROG
#include "userprog/process.h"
#endif

// ===========================================================================
// Preprocessing macros for debugging - added by our team
// ===========================================================================
// __FILE__ = built-in C macro giving string file name
// __LINE__ = built-in C macro giving integer line of code

// Format: 
//     LOGD(__LINE__,"nameOfMyFunction", varname);
//
// Sample uses:
//     LOGD(__LINE__,"sleep",thread_mlfqs);  // prints a number in %d format
//     LOGS(__LINE__,"sleep","your msg");    // prints any string
//     LOGLINE();                            // prints an empty line 
//
// Use DEBUG 1 to turn on logging
// Use DEBUG 0 to turn off logging
//
#define DEBUG 0
#if DEBUG
  #define LOGD(n,f,x) printf("DEBUG=" __FILE__ " " f "(%d): " #x " = %d\n", n,x)
  #define LOGS(n,f,x) printf("DEBUG=" __FILE__ " " f "(%d): " x "\n", n,x)
  #define LOGLINE() printf("\n")
#else
  #define LOGD(n,f,x) (void*)0
  #define LOGS(n,f,x) (void*)0
  #define LOGLINE() (void*)0
#endif

// ===========================================================================

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
    void *eip;                  /* Return address. */
    thread_func *function;      /* Function to call. */
    void *aux;                  /* Auxiliary data for function. */
  };

/* Statistics. */
static long long idle_ticks;    /* # of timer ticks spent idle. */
static long long kernel_ticks;  /* # of timer ticks in kernel threads. */
static long long user_ticks;    /* # of timer ticks in user programs. */

/* Scheduling. */
#define TIME_SLICE 4            /* # of timer ticks to give each thread. */
static unsigned thread_ticks;   /* # of timer ticks since last yield. */

/* If false (default), use round-robin scheduler.
   If true, use multi-level feedback queue scheduler.
   Controlled by kernel command-line option "-o mlfqs". */
bool thread_mlfqs;

static void kernel_thread (thread_func *, void *aux);

static void idle (void *aux UNUSED);
static struct thread *running_thread (void);
static struct thread *next_thread_to_run (void);
static void init_thread (struct thread *, const char *name, int priority);
static bool is_thread (struct thread *) UNUSED;
static void *alloc_frame (struct thread *, size_t size);
static void schedule (void);
void thread_schedule_tail (struct thread *prev);
static tid_t allocate_tid (void);

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
void
thread_init (void) 
{
  ASSERT (intr_get_level () == INTR_OFF);

  lock_init (&tid_lock);
  list_init (&ready_list);
  list_init (&all_list);

  /* Set up a thread structure for the running thread. */
  initial_thread = running_thread ();
  init_thread (initial_thread, "main", PRI_DEFAULT);
  initial_thread->status = THREAD_RUNNING;
  initial_thread->tid = allocate_tid ();
}

/* Starts preemptive thread scheduling by enabling interrupts.
   Also creates the idle thread. */
void
thread_start (void) 
{
  /* Create the idle thread. */
  struct semaphore idle_started;
  sema_init (&idle_started, 0);
  thread_create ("idle", PRI_MIN, idle, &idle_started);

  /* Start preemptive thread scheduling. */
  intr_enable ();

  /* Wait for the idle thread to initialize idle_thread. */
  sema_down (&idle_started);
}

/* Called by the timer interrupt handler at each timer tick.
   Thus, this function runs in an external interrupt context. */
void
thread_tick (void) 
{
  struct thread *t = thread_current ();

  /* Update statistics. */
  if (t == idle_thread)
    idle_ticks++;
#ifdef USERPROG
  else if (t->pagedir != NULL)
    user_ticks++;
#endif
  else
    kernel_ticks++;

  /* Enforce preemption. */
  if (++thread_ticks >= TIME_SLICE)
    intr_yield_on_return ();
}

/* Prints thread statistics. */
void
thread_print_stats (void) 
{
  printf ("Thread: %lld idle ticks, %lld kernel ticks, %lld user ticks\n",
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
tid_t
thread_create (const char *name, int priority,
               thread_func *function, void *aux) 
{
  struct thread *t;
  struct kernel_thread_frame *kf;
  struct switch_entry_frame *ef;
  struct switch_threads_frame *sf;
  tid_t tid;
  enum intr_level old_level;

  ASSERT (function != NULL);

  /* Allocate thread. */
  t = palloc_get_page (PAL_ZERO);
  if (t == NULL)
    return TID_ERROR;

  /* Initialize thread. */
  init_thread (t, name, priority);
  tid = t->tid = allocate_tid ();

  /* Prepare thread for first run by initializing its stack.
     Do this atomically so intermediate values for the 'stack' 
     member cannot be observed. */
  old_level = intr_disable ();

  /* Stack frame for kernel_thread(). */
  kf = alloc_frame (t, sizeof *kf);
  kf->eip = NULL;
  kf->function = function;
  kf->aux = aux;

  /* Stack frame for switch_entry(). */
  ef = alloc_frame (t, sizeof *ef);
  ef->eip = (void (*) (void)) kernel_thread;

  /* Stack frame for switch_threads(). */
  sf = alloc_frame (t, sizeof *sf);
  sf->eip = switch_entry;
  sf->ebp = 0;

  intr_set_level (old_level);

  /* Add to run queue. */
  thread_unblock (t);

  thread_yield_to_higher_priority_();//E&H: Added this 10.01.1981

  return tid;
}

/* Puts the current thread to sleep.  It will not be scheduled
   again until awoken by thread_unblock().

   This function must be called with interrupts turned off.  It
   is usually a better idea to use one of the synchronization
   primitives in synch.h. */
void
thread_block (void) 
{
  ASSERT (!intr_context ());
  ASSERT (intr_get_level () == INTR_OFF);

  thread_current ()->status = THREAD_BLOCKED;
  schedule ();
}

/* Transitions a blocked thread T to the ready-to-run state.
   This is an error if T is not blocked.  (Use thread_yield() to
   make the running thread ready.)

   This function does not preempt the running thread.  This can
   be important: if the caller had disabled interrupts itself,
   it may expect that it can atomically unblock a thread and
   update other data. */
void
thread_unblock (struct thread *t) 
{
  enum intr_level old_level;

  ASSERT (is_thread (t));

  old_level = intr_disable ();
  ASSERT (t->status == THREAD_BLOCKED);
  list_push_back (&ready_list, &t->elem);//9.23.2012 E&H When thread is being unblaocked, adds thread to ready list
  t->status = THREAD_READY;
  intr_set_level (old_level);
  //thread_yield_to_higher_priority_();//9.23.2012 E&H We are attempting to yield threat to higher priority after added to ready list
 
}

/* Returns the name of the running thread. */
const char *
thread_name (void) 
{
  return thread_current ()->name;
}

/* Returns the running thread.
   This is running_thread() plus a couple of sanity checks.
   See the big comment at the top of thread.h for details. */
struct thread *
thread_current (void) 
{
  struct thread *t = running_thread ();
  
  /* Make sure T is really a thread.
     If either of these assertions fire, then your thread may
     have overflowed its stack.  Each thread has less than 4 kB
     of stack, so a few big automatic arrays or moderate
     recursion can cause stack overflow. */
  ASSERT (is_thread (t));
  ASSERT (t->status == THREAD_RUNNING);

  return t;
}

/* Returns the running thread's tid. */
tid_t
thread_tid (void) 
{
  return thread_current ()->tid;
}

/* Deschedules the current thread and destroys it.  Never
   returns to the caller. */
void
thread_exit (void) 
{
  ASSERT (!intr_context ());

#ifdef USERPROG
  process_exit ();
#endif

  /* Remove thread from all threads list, set our status to dying,
     and schedule another process.  That process will destroy us
     when it calls thread_schedule_tail(). */
  intr_disable ();
  list_remove (&thread_current()->allelem);
  thread_current ()->status = THREAD_DYING;
  schedule ();
  NOT_REACHED ();
}

/* Yields the CPU.  The current thread is not put to sleep and
   may be scheduled again immediately at the scheduler's whim. */
void
thread_yield (void) 
{
  struct thread *cur = thread_current ();
  enum intr_level old_level;
  
  ASSERT (!intr_context ());

  old_level = intr_disable ();
  if (cur != idle_thread) 
    list_push_back (&ready_list, &cur->elem);
  cur->status = THREAD_READY;
  schedule ();
  intr_set_level (old_level);
}

/* Invoke function 'func' on all threads, passing along 'aux'.
   This function must be called with interrupts off. */
void
thread_foreach (thread_action_func *func, void *aux)
{
  struct list_elem *e;

  ASSERT (intr_get_level () == INTR_OFF);

  for (e = list_begin (&all_list); e != list_end (&all_list);
       e = list_next (e))
    {
      struct thread *t = list_entry (e, struct thread, allelem);
      func (t, aux);
    }
}

/* Sets the current thread's priority to NEW_PRIORITY. */
void
thread_set_priority (int new_priority) 
{
 /* Sets the thread's base priority. The thread's effective priority 
   becomes the higher of the newly set priority or the highest donated 
   priority. When the donations are released, the thread's priority becomes 
   the one set through the function call. This behavior is checked by the 
   priority-donate-lower test. - from documentation DMC*/ 

  // The thread's effective priority is the higher of the newly set priority 
  // or the highest donated priority.  If there's no donors,it has to get set.

   struct thread* c = thread_current();

   struct lock s;
   lock_init(&s);
   lock_acquire(&s);

   c ->orig_priority = new_priority; 
  
   thread_recompute_priority(c); 
   lock_release(&s);
   
   /*
   enum intr_level old_level = intr_disable();
   c->priority = new_priority;
   if (!list_empty(&c->donating_threads_list)) {
     struct thread *t = list_entry(list_max(&c->donating_threads_list, compare_donating_threads_by_priority, NULL), struct thread, donating_threads_elem);
     if (t->priority > new_priority) {
         c->priority = t->priority;
     }
  }
  
  intr_set_level(old_level);
  */
 // c->orig_priority = new_priority; // update the original priority
   
  //If the current thread no longer has the highest priority, yield to higher priority
  thread_yield_to_higher_priority_();
}

void thread_recompute_priority(struct thread *c)
{
   ASSERT (c != NULL);
   if (c->donee == NULL){ return;}

   struct lock r;
   lock_init(&r);
   lock_acquire(&r);
  
   c->priority = c->orig_priority;

   if (!list_empty(&c->donating_threads_list)) 
    {
        struct thread *max = list_entry(list_max (&c->donating_threads_list, thread_lower_priority, NULL), struct thread, donating_threads_elem);
        if (max->priority > c->priority)
        {
           c->priority =  max->priority;
        }
        if(c->priority > c->orig_priority )         
        {             
             thread_recompute_priority(c->donee);        
        }
    }
  
   lock_release(&r);
}

/* Returns the current thread's priority. */
int
thread_get_priority (void) 
{
    //9.22.2012 E&H, we need to add the ability to do the following: 
    //In the presence of priority donation, returns the higher (donated) priority
    // This should be okay - the priority is the active priority (the original
    // priority is kept in t->orig_priority.  - DMC
    return thread_current ()->priority;
}

//==========================================================================
// New functions added by our team
//==========================================================================
/* "Thread Donate Priority" gets called when a thread tries to acquire a 
   lock but is prevented by the threads acquire-lock-holding thread.  
   The donor thread passed in will try to increase the priority of the thread 
   that is holding the lock it wants to acquire. The current lock holder is 
   is included with the thread itself to support nested donation. We have tried 
   to encapsulate all information with each thread. If it gets too big, we'll 
   try again. - 10.3.12 - DMC
*/
void
thread_donate_priority(struct thread *donor)
{
   ASSERT (donor != NULL);
   ASSERT (donor->acquire_lock != NULL);

   struct thread * h = donor->donee;
   ASSERT (donor->donee !=NULL);

   //if the acquiring thread has a greater priority than the lock-holder's,
    if (donor->priority > h->priority )
    {
       h->priority = donor->priority;

       struct lock d;
       lock_init(&d);
       lock_acquire(&d);

       list_push_back (&h->donating_threads_list, &donor->donating_threads_elem);

        if (h->donee != NULL) {
             thread_donate_priority(h);
        }
      lock_release(&d);
     }
}
/* "Thread Revert Priority Donation" gets called when a thread releases a lock 
   and had benefited from priority donation.  
*/
void 
thread_revert_priority_donation(struct thread *c)
{
    ASSERT (c != NULL);
    if (c->donee == NULL) {
       return;
    }

   // struct lock v;
  //  lock_init(&v);
  //  lock_acquire(&v);

 //   list_remove(&c->donee->donating_threads_elem);
    //thread_revert_priority_donation(c->donee);
 //   lock_release(&v);

}

bool 
is_precedent(struct lock * l, struct thread * t) 
{ 
    ASSERT (t != NULL);
    if (l == NULL) return false;
    struct list_elem * e;
    for (e = list_begin (&t->precedent_lock_list); e != list_end (&t->precedent_lock_list);
          e = list_next (e))
       {
         struct lock * i = list_entry (e, struct lock, lock_list_elem);
         if (l == i)  {return true;}
         return false;
       }
}

bool
compare_donating_threads_by_priority(const struct list_elem *a_, const struct list_elem *b_, void *aux UNUSED)
{
    const struct thread *a = list_entry(a_, struct thread, donating_threads_elem);
	const struct thread *b = list_entry(b_, struct thread, donating_threads_elem);
	return a->priority < b->priority; 
}

void recompute_priority(struct thread *c)
{
    int old_priority = c->priority;//E&H: 10.4.2012 We decided to save the "old" value of priority
    enum intr_level old_level = intr_disable();

    //check if donor list is empty 

       //Find maximum in donation list of donors, donated lower priority compare by thread blah blah blah
       //if donated priority of thread is greater than default prioirty, otherise donation is defulat priority 
       //if t priority is greater than old priority && donned is not null 
       //then do a recursive call 
    if (!list_empty(&c->donating_threads_list)) //Check if donor list is empty
    {
        c->priority = list_size(&c->donating_threads_list); //wasssuuuuuup
        struct thread *t = list_max(&c->donating_threads_list, compare_donating_threads_by_priority, NULL);
            //, struct thread, donating_threads_elem));
        if (t->priority > c->priority)
        {
           c->priority = t->priority;
        }
        
        if (c->priority > old_priority)//If c priority is greater than old priority then do a recursive call 
        {
            recompute_priority(c);
        }
        
    }
    intr_set_level(old_level);
}

//==========================================================================

/* Sets the current thread's nice value to NICE. */
void
thread_set_nice (int nice UNUSED) 
{
  /* Not yet implemented. */
}

/* Returns the current thread's nice value. */
int
thread_get_nice (void) 
{
  /* Not yet implemented. */
  return 0;
}

/* Returns 100 times the system load average. */
int
thread_get_load_avg (void) 
{
  /* Not yet implemented. */
  return 0;
}

/* Returns 100 times the current thread's recent_cpu value. */
int
thread_get_recent_cpu (void) 
{
  /* Not yet implemented. */
  return 0;
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
idle (void *idle_started_ UNUSED) 
{
  struct semaphore *idle_started = idle_started_;
  idle_thread = thread_current ();
  sema_up (idle_started);

  for (;;) 
    {
      /* Let someone else run. */
      intr_disable ();
      thread_block ();

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
      asm volatile ("sti; hlt" : : : "memory");
    }
}

/* Function used as the basis for a kernel thread. */
static void
kernel_thread (thread_func *function, void *aux) 
{
  ASSERT (function != NULL);

  intr_enable ();       /* The scheduler runs with interrupts off. */
  function (aux);       /* Execute the thread function. */
  thread_exit ();       /* If function() returns, kill the thread. */
}

/* Returns the running thread. */
struct thread *
running_thread (void) 
{
  uint32_t *esp;

  /* Copy the CPU's stack pointer into `esp', and then round that
     down to the start of a page.  Because `struct thread' is
     always at the beginning of a page and the stack pointer is
     somewhere in the middle, this locates the curent thread. */
  asm ("mov %%esp, %0" : "=g" (esp));
  return pg_round_down (esp);
}

/* Returns true if T appears to point to a valid thread. */
static bool
is_thread (struct thread *t)
{
  return t != NULL && t->magic == THREAD_MAGIC;
}

/* Does basic initialization of T as a blocked thread named
   NAME. */
static void
init_thread (struct thread *t, const char *name, int priority)
{
  ASSERT (t != NULL);
  ASSERT (PRI_MIN <= priority && priority <= PRI_MAX);
  ASSERT (name != NULL);

  memset (t, 0, sizeof *t);
  t->status = THREAD_BLOCKED;
  strlcpy (t->name, name, sizeof t->name);
  t->stack = (uint8_t *) t + PGSIZE;
  t->priority = priority;
  t->magic = THREAD_MAGIC;

  t->orig_priority = t->priority;            // initialize original priority
  sema_init(&t->timer_semaphore,0);          // initialize wait timer semaphore to 0
  list_init(&t->precedent_lock_list);        // initialize the list of precedent locks
  list_init(&t->donating_threads_list);      // initialize the list of threads that have donated
  t->acquire_lock;                           // set the lock used while acquiring a lock to null
  list_push_back (&all_list, &t->allelem);
}

/* Allocates a SIZE-byte frame at the top of thread T's stack and
   returns a pointer to the frame's base. */
static void *
alloc_frame (struct thread *t, size_t size) 
{
  /* Stack data is always allocated in word-size units. */
  ASSERT (is_thread (t));
  ASSERT (size % sizeof (uint32_t) == 0);

  t->stack -= size;
  return t->stack;
}

/* Chooses and returns the next thread to be scheduled.  Should
   return a thread from the run queue, unless the run queue is
   empty.  (If the running thread can continue running, then it
   will be in the run queue.)  If the run queue is empty, return
   idle_thread. */
static struct thread *
next_thread_to_run (void)
{
  if (list_empty(&ready_list)){
    return idle_thread;
  }
  else{
    //return list_entry (list_pop_front(&ready_list), struct thread, elem); //This is what was originally here
    //Below is our new code. //E&H 9.22.12

   //I checked this section with Neilson. The struct thread, list remove, and return t are all correct as of 9.25.12
    //This gets the highest entry in the ready list, then removes that element from the list, and sends the thread
   //off to be a happy member of the running thread society. They eat ice cream on wednesdays, and enjoy listening
   //to classical music in their free time.
    struct thread *t = list_entry(list_max(&ready_list, thread_lower_priority, NULL), struct thread, elem);
    list_remove(&t->elem);
    return t;
  }
}


//9.22.12 E&H  This is the function we added for helping sort the priorities in the 
//list for the return in the method above this one, the next_thread_to_run.
bool
thread_lower_priority(const struct list_elem *a_, const struct list_elem *b_, void *aux UNUSED)
{
    const struct thread *a = list_entry(a_, struct thread, elem);
	const struct thread *b = list_entry(b_, struct thread, elem);
	return a->priority < b->priority;
}

//9.23.12 E&H This is a function we found in the lectures. 
/* If the read list contains a thread with a higher priority, 
 * yields to it. */
void thread_yield_to_higher_priority_(void)
{
    enum intr_level old_level = intr_disable();
    if (!list_empty(&ready_list)) 
    {
        struct thread *cur = thread_current();
        struct thread *max = list_entry(list_max (&ready_list, thread_lower_priority, NULL), struct thread, elem);
        if (max->priority > cur->priority)
        {
            if(intr_context())
                intr_yield_on_return();
            else
                thread_yield();
        }
    }
    intr_set_level(old_level);
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
void
thread_schedule_tail (struct thread *prev)
{
  struct thread *cur = running_thread ();
  
  ASSERT (intr_get_level () == INTR_OFF);

  /* Mark us as running. */
  cur->status = THREAD_RUNNING;

  /* Start new time slice. */
  thread_ticks = 0;

#ifdef USERPROG
  /* Activate the new address space. */
  process_activate ();
#endif

  /* If the thread we switched from is dying, destroy its struct
     thread.  This must happen late so that thread_exit() doesn't
     pull out the rug under itself.  (We don't free
     initial_thread because its memory was not obtained via
     palloc().) */
  if (prev != NULL && prev->status == THREAD_DYING && prev != initial_thread) 
    {
      ASSERT (prev != cur);
      palloc_free_page (prev);
    }
}

/* Schedules a new process.  At entry, interrupts must be off and
   the running process's state must have been changed from
   running to some other state.  This function finds another
   thread to run and switches to it.

   It's not safe to call printf() until thread_schedule_tail()
   has completed. */
static void
schedule (void) 
{
  struct thread *cur = running_thread ();
  struct thread *next = next_thread_to_run ();
  struct thread *prev = NULL;

  ASSERT (intr_get_level () == INTR_OFF);
  ASSERT (cur->status != THREAD_RUNNING);
  ASSERT (is_thread (next));

  if (cur != next)
    prev = switch_threads (cur, next);
  thread_schedule_tail (prev);
}

/* Returns a tid to use for a new thread. */
static tid_t
allocate_tid (void) 
{
  static tid_t next_tid = 1;
  tid_t tid;

  lock_acquire (&tid_lock);
  tid = next_tid++;
  lock_release (&tid_lock);

  return tid;
}

/* Offset of `stack' member within `struct thread'.
   Used by switch.S, which can't figure it out on its own. */
uint32_t thread_stack_ofs = offsetof (struct thread, stack);
