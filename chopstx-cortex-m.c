/*
 * chopstx-cortex-m.c - Threads and only threads: Arch specific code
 *                      for Cortex-M0/M3/M4
 *
 * Copyright (C) 2013, 2014, 2015, 2016, 2017, 2018, 2019, 2021, 2022
 *               Flying Stone Technology
 * Author: NIIBE Yutaka <gniibe@fsij.org>
 *
 * This file is a part of Chopstx, a thread library for embedded.
 *
 * Chopstx is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Chopstx is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * As additional permission under GNU GPL version 3 section 7, you may
 * distribute non-source form of the Program without the copy of the
 * GNU GPL normally required by section 4, provided you inform the
 * recipients of GNU GPL by a written offer.
 *
 */

static struct chx_thread *running_one;

static struct chx_thread *
chx_running (void)
{
  return running_one;
}

static void
chx_set_running (struct chx_thread *r)
{
  running_one = r;
}


/* Data Memory Barrier.  */
static void
chx_dmb (void)
{
  asm volatile ("dmb"  : : : "memory");
}

/* Saved registers on the stack.  */
struct chx_stack_regs {
  uint32_t reg[8];	       /* r0, r1, r2, r3, r12, lr, pc, xpsr */
};

/*
 * Constants for ARM.
 */
#define REG_SP 8

#define REG_R0   0
#define REG_LR   5
#define REG_PC   6
#define REG_XPSR 7

#define INITIAL_XPSR 0x01000000	/* T=1 */

/*
 * Exception priority: lower has higher precedence.
 *
 * Cortex-M3
 * =====================================
 * Prio 0x30: svc
 * ---------------------
 * Prio 0x40: thread temporarily inhibiting schedule for critical region
 * ...
 * Prio 0xb0: systick, external interrupt
 * =====================================
 *
 * Cortex-M0
 * =====================================
 * Prio 0x00: thread temporarily inhibiting schedule for critical region
 * ...
 * Prio 0x40: systick, external interrupt
 * Prio 0x80: svc (not used)
 * =====================================
 */

#define CPU_EXCEPTION_PRIORITY_CLEAR         0

#if defined(__ARM_ARCH_6M__)
#define CPU_EXCEPTION_PRIORITY_INHIBIT_SCHED 0x00
/* ... */
#define CPU_EXCEPTION_PRIORITY_SYSTICK       0x40
#define CPU_EXCEPTION_PRIORITY_INTERRUPT     CPU_EXCEPTION_PRIORITY_SYSTICK
#define CPU_EXCEPTION_PRIORITY_SVC           0x80 /* No use in this arch */
#elif defined(__ARM_ARCH_7M__) || defined(__ARM_ARCH_7EM__)
#define CPU_EXCEPTION_PRIORITY_SVC           0x30

#define CPU_EXCEPTION_PRIORITY_INHIBIT_SCHED 0x40
/* ... */
#define CPU_EXCEPTION_PRIORITY_SYSTICK       0xb0
#define CPU_EXCEPTION_PRIORITY_INTERRUPT     CPU_EXCEPTION_PRIORITY_SYSTICK
#else
#error "no support for this arch"
#endif

/*
 * Lower layer architecture specific functions.
 *
 * system tick and interrupt
 */

/*
 * System tick
 */
/* SysTick registers.  */
struct SYST {
  volatile uint32_t CSR;
  volatile uint32_t RVR;
  volatile uint32_t CVR;
  const uint32_t CALIB;
};
static struct SYST *const SYST = (struct SYST *)0xE000E010;

static void
chx_systick_init_arch (void)
{
  SYST->RVR = 0;
  SYST->CVR = 0;
  SYST->CSR = 7;
}

static void
chx_systick_reload (uint32_t ticks)
{
  SYST->RVR = ticks;
  SYST->CVR = 0;  /* write (any) to clear the counter to reload.  */
  SYST->RVR = 0;
}

static uint32_t
chx_systick_get (void)
{
  return SYST->CVR;
}

static uint32_t usec_to_ticks (uint32_t usec)
{
  return usec * MHZ;
}

static uint32_t
ticks_to_usec (uint32_t ticks)
{
  return ticks / MHZ;
}

/*
 * Interrupt Handling
 */

/* NVIC: Nested Vectored Interrupt Controller.  */
struct NVIC {
  volatile uint32_t ISER[8];
  uint32_t unused1[24];
  volatile uint32_t ICER[8];
  uint32_t unused2[24];
  volatile uint32_t ISPR[8];
  uint32_t unused3[24];
  volatile uint32_t ICPR[8];
  uint32_t unused4[24];
  volatile uint32_t IABR[8];
  uint32_t unused5[56];
  volatile uint32_t IPR[60];
};

static struct NVIC *const NVIC = (struct NVIC *)0xE000E100;
#define NVIC_ISER(n)	(NVIC->ISER[n >> 5])
#define NVIC_ICER(n)	(NVIC->ICER[n >> 5])
#define NVIC_ICPR(n)	(NVIC->ICPR[n >> 5])
#define NVIC_IPR(n)	(NVIC->IPR[n >> 2])


static void
chx_enable_intr (uint8_t irq_num)
{
  NVIC_ISER (irq_num) = 1 << (irq_num & 0x1f);
}

static void
chx_clr_intr (uint8_t irq_num)
{				/* Clear pending interrupt.  */
  NVIC_ICPR (irq_num) = 1 << (irq_num & 0x1f);
}

static int
chx_disable_intr (uint8_t irq_num)
{
  int already_disabled = !!(NVIC_ICER (irq_num) & (1 << (irq_num & 0x1f)));

  NVIC_ICER (irq_num) = 1 << (irq_num & 0x1f);
  return already_disabled;
}

static void
chx_set_intr_prio (uint8_t n)
{
  unsigned int sh = (n & 3) << 3;

  NVIC_IPR (n) = (NVIC_IPR(n) & ~(0xFF << sh))
    | (CPU_EXCEPTION_PRIORITY_INTERRUPT << sh);
}

static volatile uint32_t *const ICSR = (uint32_t *)0xE000ED04;

/* Priority control.  */
static uint32_t *const AIRCR = (uint32_t *)0xE000ED0C;
static uint32_t *const SHPR2 = (uint32_t *)0xE000ED1C;
static uint32_t *const SHPR3 = (uint32_t *)0xE000ED20;

static void
chx_interrupt_controller_init (void)
{
  *AIRCR = 0x05FA0000 | ( 5 << 8); /* PRIGROUP = 5, 2-bit:2-bit. */
  *SHPR2 = (CPU_EXCEPTION_PRIORITY_SVC << 24);
  *SHPR3 = (CPU_EXCEPTION_PRIORITY_SYSTICK << 24);
}


static void
chx_cpu_sched_lock (void)
{
#if defined(__ARM_ARCH_6M__)
  asm volatile ("cpsid	i" : : : "memory");
#else
  register uint32_t tmp = CPU_EXCEPTION_PRIORITY_INHIBIT_SCHED;
  asm volatile ("msr	BASEPRI, %0" : : "r" (tmp) : "memory");
#endif
}

static void
chx_cpu_sched_unlock (void)
{
#if defined(__ARM_ARCH_6M__)
  asm volatile ("cpsie	i" : : : "memory");
#else
  register uint32_t tmp = CPU_EXCEPTION_PRIORITY_CLEAR;
  asm volatile ("msr	BASEPRI, %0" : : "r" (tmp) : "memory");
#endif
}


static void __attribute__ ((naked))
involuntary_context_switch (struct chx_thread *running,
			    struct chx_thread *tp_next)
{
  /* Save registers onto CHX_THREAD struct.  */
  asm volatile ("adds	%0, #20\n\t"
		"stm	%0!, {r4, r5, r6, r7}\n\t"
		"mov	r2, r8\n\t"
		"mov	r3, r9\n\t"
		"mov	r4, r10\n\t"
		"mov	r5, r11\n\t"
		"mrs	r6, PSP\n\t" /* r13(=SP) in user space.  */
		"stm	%0!, {r2, r3, r4, r5, r6}\n\t"
		"mov	%0, %2"
		: "=r" (running)
		: "0" (running), "r" (tp_next)
		  /*
		   * Memory clobber constraint here is not accurate, but this
		   * works.  R7 keeps its value, but having "r7" here prevents
		   * use of R7 before this asm statement.
		   */
		: "cc", "r2", "r3", "r4", "r5", "r6", "r7", "memory");

  /* Registers on stack (PSP): r0, r1, r2, r3, r12, lr, pc, xpsr */

  asm volatile (
	/* Now, r0 points to the thread to be switched.  */
	/* Put it to *running_one.  */
	"ldr	r1, =running_one\n\t"
	/* Update running: chx_set_running */
	"str	%0, [r1]\n\t"
	/**/
	"adds	%0, #20\n\t"
	"ldm	%0!, {r4, r5, r6, r7}\n\t"
#if defined(__ARM_ARCH_6M__)
	"ldm	%0!, {r1, r2, r3}\n\t"
	"mov	r8, r1\n\t"
	"mov	r9, r2\n\t"
	"mov	r10, r3\n\t"
	"ldm	%0!, {r1, r2}\n\t"
	"mov	r11, r1\n\t"
	"msr	PSP, r2\n\t"
#else
	"ldr	r8, [%0], #4\n\t"
	"ldr	r9, [%0], #4\n\t"
	"ldr	r10, [%0], #4\n\t"
	"ldr	r11, [%0], #4\n\t"
	"ldr	r1, [%0], #4\n\t"
	"msr	PSP, r1\n\t"
#endif
	"movs	%0, #0\n\t"
	"subs	%0, #3\n\t" /* EXC_RETURN to a thread with PSP */
	"bx	%0"
	: /* no output */ : "r" (running) : "memory");
}


void __attribute__ ((naked))
chx_handle_intr (void)
{
  register uint32_t irq_num asm ("r1");
  struct chx_thread *tp_next;
  struct chx_thread *running = chx_running ();

  asm volatile ("mrs	%0, IPSR\n\t"
		/* Exception # - 16 = interrupt number.  */
		"subs	%0, #16"
		: "=r" (irq_num) : /* no input */ : "cc");

  if (irq_num == 0)
    tp_next = chx_timer_expired (running);
  else
    tp_next = chx_recv_irq (running, irq_num);

  if (tp_next)
    involuntary_context_switch (running, tp_next);
  else
    asm volatile (
	"movs	r0, #0\n\t"
	"subs	r0, #3\n\t" /* EXC_RETURN to a thread with PSP */
	"bx	r0"
	: /*no input */ : /* no input */ : "memory");
}

static void
chx_init_arch (struct chx_thread *tp)
{
  memset (&tp->tc, 0, sizeof (tp->tc));
  chx_set_running (tp);
}


/* Called with tp->lock held, resume with ->lock released.  */
static uintptr_t
voluntary_context_switch (struct chx_thread *running,
			  struct chx_thread *tp_next_arg)
{
  register uintptr_t result asm ("r0");
  register struct chx_thread *tp_next asm ("r1");
#if defined(__ARM_ARCH_7M__) || defined(__ARM_ARCH_7EM__)
  asm volatile (
	"svc	#0\n\t"
	"add	%1, %0, #16\n\t"
	"ldr	%0, [%1]"	/* Get tp->v */
	: "=r" (result), "=r" (tp_next)
	: "0" (running), "1" (tp_next_arg): "memory");
#else
  /* Build stack data as if it were an exception entry.
   * And set the stack top to have RUNNNING.
   */
  /*
   * r0:  RUNNING               scratch
   * r1:  0                     scratch
   * r2:  0                     scratch
   * r3:  0                     scratch
   * r12: 0                     scratch
   * lr   as-is
   * pc:  return address (= .L_CONTEXT_SWITCH_FINISH)
   * psr: INITIAL_XPSR          scratch
   */
  asm ("mov	r2, lr\n\t"
       "ldr	r3, =.L_CONTEXT_SWITCH_FINISH\n\t"
       "movs	r4, #128\n\t"
       "lsls	r4, #17\n\t"
       "push	{r2, r3, r4}\n\t"
       "movs	r2, #0\n\t"
       "mov	r3, r2\n\t"
       "mov	r4, r2\n\t"
       "push	{r2, r3, r4}\n\t"
       "push	{%1, r2}\n\t"
       : "=r" (tp_next)
       : "r" (running), "0" (tp_next_arg)
       : "cc", "r2", "r3", "r4", "memory");

  /* Save registers onto CHX_THREAD struct.  */
  asm ("adds	%0, #20\n\t"
       "stm	%0!, {r4, r5, r6, r7}\n\t"
       "mov	r2, r8\n\t"
       "mov	r3, r9\n\t"
       "mov	r4, r10\n\t"
       "mov	r5, r11\n\t"
       "mov	r6, sp\n\t"
       "stm	%0!, {r2, r3, r4, r5, r6}\n\t"
       "mov	%0, %2"
       : "=r" (running)
       : "0" (running), "r" (tp_next)
       : "cc", "r2", "r3", "r4", "r5", "r6", "r7", "memory");

  asm volatile (/* Now, r0 points to the thread to be switched.  */
		/* Put it to *running_one.  */
		"ldr	r1, =running_one\n\t"
		/* Update running.  */
		"str	r0, [r1]\n\t"
		"cmp	r0, #0\n\t"
		"bne	0f\n\t"

		/* Spawn an IDLE thread.  */
		"ldr	r1, =__main_stack_end__\n\t"
		"mov	sp, r1\n\t"
		"ldr	r0, =chx_idle\n\t" /* PC = idle */
		/**/
		/* Unmask interrupts.  */
		"cpsie	i\n\t"
		"bx	r0\n"

		/* Normal context switch */
	"0:\n\t"
		"adds	r0, #20\n\t"
		"ldm	r0!, {r4, r5, r6, r7}\n\t"
		"ldm	r0!, {r1, r2, r3}\n\t"
		"mov	r8, r1\n\t"
		"mov	r9, r2\n\t"
		"mov	r10, r3\n\t"
		"ldm	r0!, {r1, r2}\n\t"
		"mov	r11, r1\n\t"
		"mov	sp, r2\n\t"
		/**/
		/* Unmask interrupts.  */
		"cpsie	i\n\t"
		/*
		  0:  r0
		  4:  r1
		  8:  r2
		  12: r3
		  16: r12
		  20: lr
		  24: pc
		  28: psr
		  32: possibly exists for alignment
		  [28 or 32] <-- pc
		*/
		"ldr	r0, [sp, #28]\n\t"
		"lsls	r1, r0, #23\n\t"
		"bcc	2f\n\t"
		/**/
		"ldr	r2, [sp, #24]\n\t"
		"movs	r1, #1\n\t"
		"orrs	r2, r1\n\t"	/* Ensure Thumb-mode */
		"str	r2, [sp, #32]\n\t"
		"msr	APSR_nzcvq, r0\n\t"
		/**/
		"ldr	r0, [sp, #20]\n\t"
		"mov	lr, r0\n\t"
		"ldr	r0, [sp, #16]\n\t"
		"mov	r12, r0\n\t"
		"pop	{r0, r1, r2, r3}\n\t"
		"add	sp, #16\n\t"
		"pop	{pc}\n"
	"2:\n\t"
		"ldr	r2, [sp, #24]\n\t"
		"movs	r1, #1\n\t"
		"orrs	r2, r1\n\t"	/* Ensure Thumb-mode */
		"str	r2, [sp, #28]\n\t"
		"msr	APSR_nzcvq, r0\n\t"
		/**/
		"ldr	r0, [sp, #20]\n\t"
		"mov	lr, r0\n\t"
		"ldr	r0, [sp, #16]\n\t"
		"mov	r12, r0\n\t"
		"pop	{r0, r1, r2, r3}\n\t"
		"add	sp, #12\n\t"
		"pop	{pc}\n\t"
	".L_CONTEXT_SWITCH_FINISH:\n\t"
		"adds	r0, #16\n\t"
		"ldr	r0, [r0]"       /* Get tp->v */
		: "=r" (result)		/* Return value in R0 */
		: "0" (running)
		: "cc", "memory");
#endif
  return result;
}

extern void cause_link_time_error_unexpected_size_of_struct_chx_thread (void);

static struct chx_thread *
chopstx_create_arch (uintptr_t stack_addr, size_t stack_size,
		     voidfunc thread_entry, void *arg)
{
  struct chx_thread *tp;
  void *stack;
  struct chx_stack_regs *p;

  if (CHOPSTX_THREAD_SIZE != sizeof(struct chx_thread))
    cause_link_time_error_unexpected_size_of_struct_chx_thread ();

  if (stack_size < sizeof (struct chx_thread) + 8 * sizeof (uint32_t))
    chx_fatal (CHOPSTX_ERR_THREAD_CREATE);

  stack = (void *)(stack_addr + stack_size - sizeof (struct chx_thread)
		   - sizeof (struct chx_stack_regs));
  memset (stack, 0, sizeof (struct chx_stack_regs));
  tp = (struct chx_thread *)(stack + sizeof (struct chx_stack_regs));
  p = (struct chx_stack_regs *)stack;
  p->reg[REG_R0] = (uint32_t)arg;
  p->reg[REG_LR] = (uint32_t)chopstx_exit;
  p->reg[REG_PC] = (uint32_t)thread_entry;
  p->reg[REG_XPSR] = INITIAL_XPSR;

  memset (&tp->tc, 0, sizeof (tp->tc));
  tp->tc.reg[REG_SP] = (uint32_t)stack;

  return tp;
}


#if defined(__ARM_ARCH_7M__) || defined(__ARM_ARCH_7EM__)
/*
 * System call: switch to another thread.
 */
void __attribute__ ((naked))
svc (void)
{
  register uint32_t tp_next asm ("r0");

  asm ("adds	r0, #20\n\t"
       /* Save registers onto CHX_THREAD struct.  */
       "stm	r0!, {r4, r5, r6, r7}\n\t"
       "mov	r2, r8\n\t"
       "mov	r3, r9\n\t"
       "mov	r4, r10\n\t"
       "mov	r5, r11\n\t"
       "mrs	r6, PSP\n\t" /* r13(=SP) in user space.  */
       "stm	r0!, {r2, r3, r4, r5, r6}\n\t"
       "subs	r0, #56\n\t"
       "str	r0, [r6]\n\t"
       "mov	r0, r1"
       : "=r" (tp_next)
       : /* no input */
       : "cc", "r1", "r2", "r3", "r4", "r5", "r6", "memory");

  asm volatile (
	/* Now, r0 points to the thread to be switched.  */
	/* Put it to *running.  */
	"ldr	r1, =running_one\n\t"
	/* Update running: chx_set_running */
	"str	r0, [r1]\n\t"
	"cbz	r0, 1f\n\t"
	/**/
	"adds	r0, #20\n\t"
	"ldm	r0!, {r4, r5, r6, r7}\n\t"
	"ldr	r8, [r0], #4\n\t"
	"ldr	r9, [r0], #4\n\t"
	"ldr	r10, [r0], #4\n\t"
	"ldr	r11, [r0], #4\n\t"
	"ldr	r1, [r0], #4\n\t"
	"msr	PSP, r1\n\t"
	/* Unmask interrupts.  */
	"movs	r0, #0\n\t"
	"msr	BASEPRI, r0\n\t"
	"subs	r0, #3\n\t" /* EXC_RETURN to a thread with PSP */
	"bx	r0\n"
    "1:\n\t"
	/* Spawn an IDLE thread.  */
	"ldr	r0, =__main_stack_end__-32\n\t"
	"msr	PSP, r0\n\t"
	"movs	r1, #0\n\t"
	"movs	r2, #0\n\t"
	"movs	r3, #0\n\t"
	"stm	r0!, {r1, r2, r3}\n\t"
	"stm	r0!, {r1, r2, r3}\n\t"
	"ldr	r1, =chx_idle\n\t" /* PC = idle */
	"movs	r2, #0x010\n\t"
	"lsls	r2, r2, #20\n\t" /* xPSR = T-flag set (Thumb) */
	"stm	r0!, {r1, r2}\n\t"
	/* Unmask interrupts.  */
	"movs	r0, #0\n\t"
	"msr	BASEPRI, r0\n"
	"subs	r0, #3\n\t" /* EXC_RETURN to a thread with PSP */
	"bx	r0"
	: /* no output */ : "r" (tp_next) : "memory");
}
#endif
