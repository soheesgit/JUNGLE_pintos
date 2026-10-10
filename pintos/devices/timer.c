#include "devices/timer.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include "threads/interrupt.h"
#include "threads/io.h"
#include "threads/synch.h"
#include "threads/thread.h"
#include "lib/kernel/list.h"//리스트 자료형 사용을 위한 헤더 파일

/* See [8254] for hardware details of the 8254 timer chip. */

#if TIMER_FREQ < 19
#error 8254 timer requires TIMER_FREQ >= 19
#endif
#if TIMER_FREQ > 1000
#error TIMER_FREQ <= 1000 recommended
#endif

/* Number of timer ticks since OS booted. */
static int64_t ticks;

/* Number of loops per timer tick.
   Initialized by timer_calibrate(). */
static unsigned loops_per_tick;

static intr_handler_func timer_interrupt;
static bool too_many_loops (unsigned loops);
static void busy_wait (int64_t loops);
static void real_time_sleep (int64_t num, int32_t denom);

static struct list sleep_list;//잠자는 thread list

/* Sets up the 8254 Programmable Interval Timer (PIT) to
   interrupt PIT_FREQ times per second, and registers the
   corresponding interrupt. */
void
timer_init (void) {
	/* 8254 input frequency divided by TIMER_FREQ, rounded to
	   nearest. */
	   list_init (&sleep_list);//목록 초기화
	uint16_t count = (1193180 + TIMER_FREQ / 2) / TIMER_FREQ;

	outb (0x43, 0x34);    /* CW: counter 0, LSB then MSB, mode 2, binary. */
	outb (0x40, count & 0xff);
	outb (0x40, count >> 8);

	intr_register_ext (0x20, timer_interrupt, "8254 Timer");
}

/* Calibrates loops_per_tick, used to implement brief delays. */
void
timer_calibrate (void) {
	unsigned high_bit, test_bit;

	ASSERT (intr_get_level () == INTR_ON);
	printf ("Calibrating timer...  ");

	/* Approximate loops_per_tick as the largest power-of-two
	   still less than one timer tick. */
	loops_per_tick = 1u << 10;
	while (!too_many_loops (loops_per_tick << 1)) {
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
timer_ticks (void) {
	enum intr_level old_level = intr_disable ();
	int64_t t = ticks;
	intr_set_level (old_level);
	barrier ();
	return t;
}

/* Returns the number of timer ticks elapsed since THEN, which
   should be a value once returned by timer_ticks(). */
int64_t
timer_elapsed (int64_t then) {
	return timer_ticks () - then;
}

/* Suspends execution for approximately TICKS timer ticks. */
void
timer_sleep (int64_t delay) {
	// int64_t start = timer_ticks (); 필요X

	ASSERT (intr_get_level () == INTR_ON);
	// while (timer_elapsed (start) < ticks)
	// 	thread_yield (); 문제의 코드
	
	/*기다릴 시간이 0 이하일 경우 잠들지 않고 즉시 반환*/
	if(delay <= 0)
	return;

	enum intr_level old_intr = intr_disable();//타이머 인터럽트가 끼어들지 않도록 현재 인터럽트 상태를 저장하고 종료
	struct thread *current = thread_current();//현재 실행 중인 쓰레드의 구조체를 가리키는 포인터를 가져옴

	/* 현재 타이머 시각에 기다릴 tick수를 더해 이 쓰레드가 깨어나야 할 '목표' 시각을 기록 */
	current->wakeup_t = timer_ticks() + delay;

	/* list.c/list_push_back 함수(리스트 항목을 넣고 빼거나, 순회하는 핀토스 내장 함수)를 사용하여
	현재 쓰레드의 리스트 요소를 sleep_list 끝에 추가 */
	list_push_back (&sleep_list, &current->elem);
	
	thread_block();//쓰레드를 잠들게 만들어 대기 시킨다

	/* 타이머 처리에서 이 스레드를 깨워 재실행시 함수가 잠들기 전에 저장해 둔 intr 상태를 복구 */
	intr_set_level (old_intr);
}

/* Suspends execution for approximately MS milliseconds. */
void
timer_msleep (int64_t ms) {
	real_time_sleep (ms, 1000);
}

/* Suspends execution for approximately US microseconds. */
void
timer_usleep (int64_t us) {
	real_time_sleep (us, 1000 * 1000);
}

/* Suspends execution for approximately NS nanoseconds. */
void
timer_nsleep (int64_t ns) {
	real_time_sleep (ns, 1000 * 1000 * 1000);
}

/* Prints timer statistics. */
void
timer_print_stats (void) {
	printf ("Timer: %"PRId64" ticks\n", timer_ticks ());
}

/* Timer interrupt handler. */
/* [SHARED] Alarm/MLFQS: 수면 스레드 깨우기와 주기 갱신의 순서 검토. Jira: KAN-78 */
static void
timer_interrupt (struct intr_frame *args UNUSED) {//중복 함수
	ticks++;
	thread_tick ();
	
	//기상 시각에 도달했거나 지난 잠자는 쓰레드 찾아서 깨우는 기능
	//->
	struct list_elem *e = list_begin (&sleep_list);//잠자는 쓰레드 목록을 순회할 때 현재 위치를 가리킨다
	
	while (e->next==NULL)/*sleep_list 전체를 순회 종료조건 : */
	{
		struct thread *wt = list_entry(list_begin(&sleep_list), struct thread, elem);
		if(wt->wakeup_t<=ticks)
		{
			e=list_remove(e);
			thread_unblock(wt);
			
		}else
		{
			e=list_next(e);
		}

	}
	
}

/* Returns true if LOOPS iterations waits for more than one timer
   tick, otherwise false. */
static bool
too_many_loops (unsigned loops) {
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
busy_wait (int64_t loops) {
	while (loops-- > 0)
		barrier ();
}

/* Sleep for approximately NUM/DENOM seconds. */
static void
real_time_sleep (int64_t num, int32_t denom) {
	/* Convert NUM/DENOM seconds into timer ticks, rounding down.

	   (NUM / DENOM) s
	   ---------------------- = NUM * TIMER_FREQ / DENOM ticks.
	   1 s / TIMER_FREQ ticks
	   */
	int64_t ticks = num * TIMER_FREQ / denom;

	ASSERT (intr_get_level () == INTR_ON);
	if (ticks > 0) {
		/* We're waiting for at least one full timer tick.  Use
		   timer_sleep() because it will yield the CPU to other
		   processes. */
		timer_sleep (ticks);
	} else {
		/* Otherwise, use a busy-wait loop for more accurate
		   sub-tick timing.  We scale the numerator and denominator
		   down by 1000 to avoid the possibility of overflow. */
		ASSERT (denom % 1000 == 0);
		busy_wait (loops_per_tick * num / 1000 * TIMER_FREQ / (denom / 1000));
	}
}
