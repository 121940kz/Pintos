#include "devices/timer.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include "devices/pit.h"
#include "threads/interrupt.h"
#include "threads/synch.h"
#include "threads/thread.h"

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
  
/* See [8254] for hardware details of the 8254 timer chip. */

#if TIMER_FREQ < 19
#error 8254 timer requires TIMER_FREQ >= 19
#endif
#if TIMER_FREQ > 1000
#error TIMER_FREQ <= 1000 recommended
#endif

static struct list wait_list;  // timer keeps a list of threads waiting

/* Number of timer ticks since OS booted. */
static int64_t ticks;

/* Number of loops per timer tick.
   Initialized by timer_calibrate(). */
static unsigned loops_per_tick;

static intr_handler_func timer_interrupt;
static bool too_many_loops (unsigned loops);
static void busy_wait (int64_t loops);
static void real_time_sleep (int64_t num, int32_t denom);
static void real_time_delay (int64_t num, int32_t denom);

// ===========================================================================
// New - added by our team
// ===========================================================================

bool compare_threads_by_wakeup_time(const struct list_elem *a_, const struct list_elem *b_, void *aux UNUSED);

/* Compares the value of two list elements A and B, given
   auxiliary data AUX.  Returns true if A is less than B, or
   false if A is greater than or equal to B. */

bool
compare_threads_by_wakeup_time(const struct list_elem *a_, const struct list_elem *b_, void *aux UNUSED)
{
       //the list_entry elem can be shared - see thread.h

       const struct thread *a = list_entry(a_, struct thread, elem);
	const struct thread *b = list_entry(b_, struct thread, elem);

       // Debug wait_list 
       LOGD(__LINE__,"compareByWake",a->wakeup_time);
       LOGD(__LINE__,"compareByWake",b->wakeup_time);

	return a->wakeup_time <= b->wakeup_time; 
}
  //=======================================================================
  // end our modifications
  //=======================================================================

/* Sets up the timer to interrupt TIMER_FREQ times per second,
   and registers the corresponding interrupt. */
void
timer_init (void) 
{
  LOGLINE();
  LOGS(__LINE__,"timer_init","starting timer init");  

  pit_configure_channel (0, 2, TIMER_FREQ);
  intr_register_ext (0x20, timer_interrupt, "8254 Timer");
  list_init(&wait_list);
  LOGD(__LINE__,"timer_init",list_size(&wait_list));
  LOGS(__LINE__,"timer_init","done with timer init");  
  LOGLINE();
}

/* Calibrates loops_per_tick, used to implement brief delays. */
void
timer_calibrate (void) 
{
  unsigned high_bit, test_bit;

  ASSERT (intr_get_level () == INTR_ON);
  printf ("Calibrating timer...  ");

  /* Approximate loops_per_tick as the largest power-of-two
     still less than one timer tick. */
  loops_per_tick = 1u << 10;
  while (!too_many_loops (loops_per_tick << 1)) 
    {
      loops_per_tick <<= 1;
      ASSERT (loops_per_tick != 0);
    }

  /* Refine the next 8 bits of loops_per_tick. */
  high_bit = loops_per_tick;
  for (test_bit = high_bit >> 1; test_bit != high_bit >> 10; test_bit >>= 1)
    if (!too_many_loops (high_bit | test_bit))
      loops_per_tick |= test_bit;

  printf ("%'"PRIu64" loops/s.\n", (uint64_t) loops_per_tick * TIMER_FREQ);
}

/* Returns the number of timer ticks since the OS booted. */
int64_t
timer_ticks (void) 
{
  enum intr_level old_level = intr_disable ();
  int64_t t = ticks;
  intr_set_level (old_level);
  return t;
}

/* Returns the number of timer ticks elapsed since THEN, which
   should be a value once returned by timer_ticks(). */
int64_t
timer_elapsed (int64_t then) 
{
  return timer_ticks () - then;
}

/* Sleeps for approximately TICKS timer ticks.  Interrupts must
   be turned on. */
// ===========================================================================
// timer_sleep has been updated by our team:
// We remove thread from ready list by adding it to wait list.
// And block the thread by downing semaphore initialized to 0.
// ===========================================================================
void
timer_sleep (int64_t ticks) 
{
 // ORIGINAL CODE FOR TESTING.............
 // get the number of ticks since the OS booted
 // int64_t start = timer_ticks ();//creates a new timer called start   // from original code - still good

 // ASSERT (intr_get_level () == INTR_ON);
 // while (timer_elapsed (start) < ticks) 
 //   thread_yield ();
 // return;
 
  // If parameter is invalid, exit gracefully
  if (ticks < 1) { return;}

  // Assert interrupts are on when we enter
  ASSERT (intr_get_level () == INTR_ON);

 // get the current time
  int64_t start = timer_ticks ();//creates a new timer called start   // from original code - still good

  // Debug code to see where we're at when we first arrive:
  LOGLINE();                              // start with a blank debug line
  LOGD(__LINE__,"timer_sleep",ticks);     // display number of ticks to wait
  LOGD(__LINE__,"timer_sleep",start);     // display sleep starting time

  // create a pointer to a thread structure and set to the current thread
  struct thread *t = thread_current ();  // given in class

  // Just curious - what thread is our current thread?
  LOGD(__LINE__,"timer_sleep",t->tid);   // display tid of the current thread

  // Schedule our wake-up time. <---- given in class
  t->wakeup_time = start + ticks;

  // Debug wake time 
  LOGD(__LINE__,"timer_sleep",t->wakeup_time);       // display it

  // disable interrupts 
  intr_disable ();   // given in class - disable interrupts

  //Insert the current thread into the wait list. <--- from class
  list_insert_ordered (&wait_list, &t->elem, compare_threads_by_wakeup_time, NULL);

  // reenable interrupts
  intr_enable ();    // given in class - enable interrupts 

   // Debug wait_list 
  LOGD(__LINE__,"timer_sleep",list_size(&wait_list));
  
  // down the timer semaphore to block this thread until its wait time expires (pass by address)
  sema_down(&t->timer_semaphore); 
  
  //if (&t->wakeup_time <=  timer_ticks ()) {   // Try it here...
  //   sema_up(&t->timer_semaphore);
  //}


  
  //We need to remove thread from the wait list in order to wake it up
  //   Yep - we need to check the wait list with each tick and see who 
  //   needs to get woken up - see timer_interrupt. - Denise
  //t = list_pop_front(&wait_list); //Added  by Heath and Emily 9/16/2012
   
  //release items semaphore
  //sema_up(&t->timer_semaphore); //Added by Heath and Emily 9/16/2012

  //thread *s = wait_list.head;
  // if(s->wakeup_time > ticks)
  // {
  //   sema_up(timer_semaphore);
  //    wait_list.list_remove(&s);
  // }
  //=======================================================================
  // end our modifications
  //=======================================================================
}


/* Sleeps for approximately MS milliseconds.  Interrupts must be
   turned on. */
void
timer_msleep (int64_t ms) 
{
  real_time_sleep (ms, 1000);
}

/* Sleeps for approximately US microseconds.  Interrupts must be
   turned on. */
void
timer_usleep (int64_t us) 
{
  real_time_sleep (us, 1000 * 1000);
}

/* Sleeps for approximately NS nanoseconds.  Interrupts must be
   turned on. */
void
timer_nsleep (int64_t ns) 
{
  real_time_sleep (ns, 1000 * 1000 * 1000);
}

/* Busy-waits for approximately MS milliseconds.  Interrupts need
   not be turned on.

   Busy waiting wastes CPU cycles, and busy waiting with
   interrupts off for the interval between timer ticks or longer
   will cause timer ticks to be lost.  Thus, use timer_msleep()
   instead if interrupts are enabled. */
void
timer_mdelay (int64_t ms) 
{
  real_time_delay (ms, 1000);
}

/* Sleeps for approximately US microseconds.  Interrupts need not
   be turned on.

   Busy waiting wastes CPU cycles, and busy waiting with
   interrupts off for the interval between timer ticks or longer
   will cause timer ticks to be lost.  Thus, use timer_usleep()
   instead if interrupts are enabled. */
void
timer_udelay (int64_t us) 
{
  real_time_delay (us, 1000 * 1000);
}

/* Sleeps execution for approximately NS nanoseconds.  Interrupts
   need not be turned on.

   Busy waiting wastes CPU cycles, and busy waiting with
   interrupts off for the interval between timer ticks or longer
   will cause timer ticks to be lost.  Thus, use timer_nsleep()
   instead if interrupts are enabled.*/
void
timer_ndelay (int64_t ns) 
{
  real_time_delay (ns, 1000 * 1000 * 1000);
}

/* Prints timer statistics. */
void
timer_print_stats (void) 
{
  printf ("Timer: %"PRId64" ticks\n", timer_ticks ());
}

/* Timer interrupt handler. */
static void
timer_interrupt (struct intr_frame *args UNUSED)
{
  // ======================================================================
  // This function occurs each tick.  
  // Check the wait list and see who is ready to wake up.
  // If ready, remove thread elements from the wake list. 
  // Sample list iteration code from list.c ~line 139 
  //=======================================================================

    // get pointer to first item in the list (the beginning)
    struct list_elem *e = list_begin (&wait_list);

    if (e != NULL) {

      // get a pointer to the first item itself (see list.c ~line128)
      struct thread *t = list_entry(e, struct thread, elem);
  
      // while we're not at the end and it's past our wakeup time
      while ((e != list_end(&wait_list)) && (t->wakeup_time <= timer_ticks())) {

         // up the thread's timer semaphore  
         sema_up(&t->timer_semaphore);

         // remove the elem from the wait list (see list.c ~line 222 comments)
         // and advance pointer to next list item 
         e = list_remove(e);     

         // update our thread pointer to this item's thread
         t = list_entry(e, struct thread, elem); 
 
         // display
         LOGD(__LINE__,"timer_interrupt",t->tid); 
    }
  }
  //=======================================================================
  // end our modifications
  //=======================================================================

  ticks++;
  thread_tick();
}

/* Returns true if LOOPS iterations waits for more than one timer
   tick, otherwise false. */
static bool
too_many_loops (unsigned loops) 
{
  /* Wait for a timer tick. */
  int64_t start = ticks;
  while (ticks == start)
    barrier ();

  /* Run LOOPS loops. */
  start = ticks;
  busy_wait (loops);

  /* If the tick count changed, we iterated too long. */
  barrier ();
  return start != ticks;
}

/* Iterates through a simple loop LOOPS times, for implementing
   brief delays.

   Marked NO_INLINE because code alignment can significantly
   affect timings, so that if this function was inlined
   differently in different places the results would be difficult
   to predict. */
static void NO_INLINE
busy_wait (int64_t loops) 
{
  while (loops-- > 0)
    barrier ();
}

/* Sleep for approximately NUM/DENOM seconds. */
static void
real_time_sleep (int64_t num, int32_t denom) 
{
  /* Convert NUM/DENOM seconds into timer ticks, rounding down.
          
        (NUM / DENOM) s          
     ---------------------- = NUM * TIMER_FREQ / DENOM ticks. 
     1 s / TIMER_FREQ ticks
  */
  int64_t ticks = num * TIMER_FREQ / denom;

  ASSERT (intr_get_level () == INTR_ON);
  if (ticks > 0)
    {
      /* We're waiting for at least one full timer tick.  Use
         timer_sleep() because it will yield the CPU to other
         processes. */                
      timer_sleep (ticks); 
    }
  else 
    {
      /* Otherwise, use a busy-wait loop for more accurate
         sub-tick timing. */
      real_time_delay (num, denom); 
    }
}

/* Busy-wait for approximately NUM/DENOM seconds. */
static void
real_time_delay (int64_t num, int32_t denom)
{
  /* Scale the numerator and denominator down by 1000 to avoid
     the possibility of overflow. */
  ASSERT (denom % 1000 == 0);
  busy_wait (loops_per_tick * num / 1000 * TIMER_FREQ / (denom / 1000)); 
}

