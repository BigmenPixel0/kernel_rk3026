/*
 * arch/arm/common/fiq_debugger.c
 *
 * Serial Debugger Interface accessed through an FIQ interrupt.
 *
 * Copyright (C) 2008 Google, Inc.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <stdarg.h>
#include <linux/module.h>
#include <linux/io.h>
#include <linux/console.h>
#include <linux/interrupt.h>
#include <linux/clk.h>
#include <linux/platform_device.h>
#include <linux/kernel_stat.h>
#include <linux/irq.h>
#include <linux/delay.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/smp.h>
#include <linux/timer.h>
#include <linux/tty.h>
#include <linux/tty_flip.h>
#include <linux/wakelock.h>

#include <asm/fiq_debugger.h>
#include <asm/fiq_glue.h>
#include <asm/stacktrace.h>

#include <mach/system.h>

#include <linux/uaccess.h>

#include "fiq_debugger_ringbuf.h"

#ifdef CONFIG_RK29_WATCHDOG
extern void rk29_wdt_keepalive(void);
#define wdt_keepalive() rk29_wdt_keepalive()
#else
#define wdt_keepalive() do {} while (0)
#endif

#define DEBUG_MAX 64
#define CMD_COUNT 0x0f
#define MAX_UNHANDLED_FIQ_COUNT 1000000

#define THREAD_INFO(sp) ((struct thread_info *) \
		((unsigned long)(sp) & ~(THREAD_SIZE - 1)))

struct fiq_debugger_state {
	struct fiq_glue_handler handler;

	int fiq;
	int uart_irq;
	int signal_irq;
	int wakeup_irq;
	bool wakeup_irq_no_set_wake;
	struct clk *clk;
	struct fiq_debugger_pdata *pdata;
	struct platform_device *pdev;

	char debug_cmd[DEBUG_MAX];
	int debug_busy;
	int debug_abort;

	char debug_buf[DEBUG_MAX];
	int debug_count;

	char cmd_buf[CMD_COUNT+1][DEBUG_MAX];
	int back_pointer;
	int current_pointer;

	bool no_sleep;
	bool debug_enable;
	bool ignore_next_wakeup_irq;
	struct timer_list sleep_timer;
	spinlock_t sleep_timer_lock;
	bool uart_enabled;
	struct wake_lock debugger_wake_lock;
	bool console_enable;
	int current_cpu;
	atomic_t unhandled_fiq_count;
	bool in_fiq;

#ifdef CONFIG_FIQ_DEBUGGER_CONSOLE
	struct console console;
	struct tty_driver *tty_driver;
	struct tty_struct *tty;
	int tty_open_count;
	struct fiq_debugger_ringbuf *tty_rbuf;
	bool syslog_dumping;
#endif

	unsigned int last_irqs[NR_IRQS];
	unsigned int last_local_timer_irqs[NR_CPUS];
};

#ifdef CONFIG_FIQ_DEBUGGER_NO_SLEEP
static bool initial_no_sleep = true;
#else
static bool initial_no_sleep;
#endif

#ifdef CONFIG_FIQ_DEBUGGER_CONSOLE_DEFAULT_ENABLE
static bool initial_debug_enable = true;
static bool initial_console_enable = true;
#else
static bool initial_debug_enable;
static bool initial_console_enable;
#endif

module_param_named(no_sleep, initial_no_sleep, bool, 0644);
module_param_named(debug_enable, initial_debug_enable, bool, 0644);
module_param_named(console_enable, initial_console_enable, bool, 0644);

#ifdef CONFIG_FIQ_DEBUGGER_WAKEUP_IRQ_ALWAYS_ON
static inline void enable_wakeup_irq(struct fiq_debugger_state *state) {}
static inline void disable_wakeup_irq(struct fiq_debugger_state *state) {}
#else
static inline void enable_wakeup_irq(struct fiq_debugger_state *state)
{
	if (state->wakeup_irq < 0)
		return;
	enable_irq(state->wakeup_irq);
	if (!state->wakeup_irq_no_set_wake)
		enable_irq_wake(state->wakeup_irq);
}
static inline void disable_wakeup_irq(struct fiq_debugger_state *state)
{
	if (state->wakeup_irq < 0)
		return;
	disable_irq_nosync(state->wakeup_irq);
	if (!state->wakeup_irq_no_set_wake)
		disable_irq_wake(state->wakeup_irq);
}
#endif

static bool inline debug_have_fiq(struct fiq_debugger_state *state)
{
	return (state->fiq >= 0);
}

static void debug_force_irq(struct fiq_debugger_state *state)
{
	unsigned int irq = state->signal_irq;

	if (WARN_ON(!debug_have_fiq(state)))
		return;
	if (state->pdata->force_irq) {
		state->pdata->force_irq(state->pdev, irq);
	} else {
		struct irq_chip *chip = irq_get_chip(irq);
		if (chip && chip->irq_retrigger)
			chip->irq_retrigger(irq_get_irq_data(irq));
	}
}

static void debug_uart_enable(struct fiq_debugger_state *state)
{
	if (state->clk)
		clk_enable(state->clk);
	if (state->pdata->uart_enable)
		state->pdata->uart_enable(state->pdev);
}

static void debug_uart_disable(struct fiq_debugger_state *state)
{
	if (state->pdata->uart_disable)
		state->pdata->uart_disable(state->pdev);
	if (state->clk)
		clk_disable(state->clk);
}

static void debug_uart_flush(struct fiq_debugger_state *state)
{
	if (state->pdata->uart_flush)
		state->pdata->uart_flush(state->pdev);
}

static void debug_puts(struct fiq_debugger_state *state, char *s)
{
	unsigned c;
	while ((c = *s++)) {
		if (c == '\n')
			state->pdata->uart_putc(state->pdev, '\r');
		state->pdata->uart_putc(state->pdev, c);
	}
}

static void debug_prompt(struct fiq_debugger_state *state)
{
	debug_puts(state, "debug> ");
}

int log_buf_copy(char *dest, int idx, int len);
static void dump_kernel_log(struct fiq_debugger_state *state)
{
	char buf[1024];
	int idx = 0;
	int ret;
	int saved_oip;

	/* setting oops_in_progress prevents log_buf_copy()
	 * from trying to take a spinlock which will make it
	 * very unhappy in some cases...
	 */
	saved_oip = oops_in_progress;
	oops_in_progress = 1;
	for (;;) {
		wdt_keepalive();
		ret = log_buf_copy(buf, idx, 1023);
		if (ret <= 0)
			break;
		buf[ret] = 0;
		debug_puts(state, buf);
		idx += ret;
	}
	oops_in_progress = saved_oip;
}

#ifdef CONFIG_RK29_LAST_LOG
#include <linux/ctype.h>
extern char *last_log_get(unsigned *size);
static void dump_last_kernel_log(struct fiq_debugger_state *state)
{
	unsigned size, i, c;
	char *s = last_log_get(&size);

	for (i = 0; i < size; i++) {
		if (i % 1024 == 0)
			wdt_keepalive();
		c = s[i];
		if (c == '\n') {
			state->pdata->uart_putc(state->pdev, '\r');
			state->pdata->uart_putc(state->pdev, c);
		} else if (isascii(c) && isprint(c)) {
			state->pdata->uart_putc(state->pdev, c);
		}
	}
}
#endif

static char *mode_name(unsigned cpsr)
{
	switch (cpsr & MODE_MASK) {
	case USR_MODE: return "USR";
	case FIQ_MODE: return "FIQ";
	case IRQ_MODE: return "IRQ";
	case SVC_MODE: return "SVC";
	case ABT_MODE: return "ABT";
	case UND_MODE: return "UND";
	case SYSTEM_MODE: return "SYS";
	default: return "???";
	}
}

static int debug_printf(void *cookie, const char *fmt, ...)
{
	struct fiq_debugger_state *state = cookie;
	char buf[256];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);

	debug_puts(state, buf);
	return state->debug_abort;
}

/* Safe outside fiq context */
static int debug_printf_nfiq(void *cookie, const char *fmt, ...)
{
	struct fiq_debugger_state *state = cookie;
	char buf[256];
	va_list ap;
	unsigned long irq_flags;

	va_start(ap, fmt);
	vsnprintf(buf, 128, fmt, ap);
	va_end(ap);

	local_irq_save(irq_flags);
	debug_puts(state, buf);
	debug_uart_flush(state);
	local_irq_restore(irq_flags);
	return state->debug_abort;
}

static void dump_regs(struct fiq_debugger_state *state, unsigned *regs)
{
	debug_printf(state, " r0 %08x  r1 %08x  r2 %08x  r3 %08x\n",
			regs[0], regs[1], regs[2], regs[3]);
	debug_printf(state, " r4 %08x  r5 %08x  r6 %08x  r7 %08x\n",
			regs[4], regs[5], regs[6], regs[7]);
	debug_printf(state, " r8 %08x  r9 %08x r10 %08x r11 %08x  mode %s\n",
			regs[8], regs[9], regs[10], regs[11],
			mode_name(regs[16]));
	if ((regs[16] & MODE_MASK) == USR_MODE)
		debug_printf(state, " ip %08x  sp %08x  lr %08x  pc %08x  "
				"cpsr %08x\n", regs[12], regs[13], regs[14],
				regs[15], regs[16]);
	else
		debug_printf(state, " ip %08x  sp %08x  lr %08x  pc %08x  "
				"cpsr %08x  spsr %08x\n", regs[12], regs[13],
				regs[14], regs[15], regs[16], regs[17]);
}

struct mode_regs {
	unsigned long sp_svc;
	unsigned long lr_svc;
	unsigned long spsr_svc;

	unsigned long sp_abt;
	unsigned long lr_abt;
	unsigned long spsr_abt;

	unsigned long sp_und;
	unsigned long lr_und;
	unsigned long spsr_und;

	unsigned long sp_irq;
	unsigned long lr_irq;
	unsigned long spsr_irq;

	unsigned long r8_fiq;
	unsigned long r9_fiq;
	unsigned long r10_fiq;
	unsigned long r11_fiq;
	unsigned long r12_fiq;
	unsigned long sp_fiq;
	unsigned long lr_fiq;
	unsigned long spsr_fiq;
};

void __naked get_mode_regs(struct mode_regs *regs)
{
	asm volatile (
	"mrs	r1, cpsr\n"
	"msr	cpsr_c, #0xd3 @(SVC_MODE | PSR_I_BIT | PSR_F_BIT)\n"
	"stmia	r0!, {r13 - r14}\n"
	"mrs	r2, spsr\n"
	"msr	cpsr_c, #0xd7 @(ABT_MODE | PSR_I_BIT | PSR_F_BIT)\n"
	"stmia	r0!, {r2, r13 - r14}\n"
	"mrs	r2, spsr\n"
	"msr	cpsr_c, #0xdb @(UND_MODE | PSR_I_BIT | PSR_F_BIT)\n"
	"stmia	r0!, {r2, r13 - r14}\n"
	"mrs	r2, spsr\n"
	"msr	cpsr_c, #0xd2 @(IRQ_MODE | PSR_I_BIT | PSR_F_BIT)\n"
	"stmia	r0!, {r2, r13 - r14}\n"
	"mrs	r2, spsr\n"
	"msr	cpsr_c, #0xd1 @(FIQ_MODE | PSR_I_BIT | PSR_F_BIT)\n"
	"stmia	r0!, {r2, r8 - r14}\n"
	"mrs	r2, spsr\n"
	"stmia	r0!, {r2}\n"
	"msr	cpsr_c, r1\n"
	"bx	lr\n");
}


static void dump_allregs(struct fiq_debugger_state *state, unsigned *regs)
{
	struct mode_regs mode_regs;
	dump_regs(state, regs);
	get_mode_regs(&mode_regs);
	debug_printf(state, " svc: sp %08x  lr %08x  spsr %08x\n",
			mode_regs.sp_svc, mode_regs.lr_svc, mode_regs.spsr_svc);
	debug_printf(state, " abt: sp %08x  lr %08x  spsr %08x\n",
			mode_regs.sp_abt, mode_regs.lr_abt, mode_regs.spsr_abt);
	debug_printf(state, " und: sp %08x  lr %08x  spsr %08x\n",
			mode_regs.sp_und, mode_regs.lr_und, mode_regs.spsr_und);
	debug_printf(state, " irq: sp %08x  lr %08x  spsr %08x\n",
			mode_regs.sp_irq, mode_regs.lr_irq, mode_regs.spsr_irq);
	debug_printf(state, " fiq: r8 %08x  r9 %08x  r10 %08x  r11 %08x  "
			"r12 %08x\n",
			mode_regs.r8_fiq, mode_regs.r9_fiq, mode_regs.r10_fiq,
			mode_regs.r11_fiq, mode_regs.r12_fiq);
	debug_printf(state, " fiq: sp %08x  lr %08x  spsr %08x\n",
			mode_regs.sp_fiq, mode_regs.lr_fiq, mode_regs.spsr_fiq);
}

static void dump_irqs(struct fiq_debugger_state *state)
{
	int n;
	unsigned int cpu;

	debug_printf(state, "irqnr       total  since-last   status  name\n");
	for (n = 0; n < NR_IRQS; n++) {
		struct irqaction *act = irq_desc[n].action;
		if (!act && !kstat_irqs(n))
			continue;
		debug_printf(state, "%5d: %10u %11u %8x  %s\n", n,
			kstat_irqs(n),
			kstat_irqs(n) - state->last_irqs[n],
			irq_desc[n].status_use_accessors,
			(act && act->name) ? act->name : "???");
		state->last_irqs[n] = kstat_irqs(n);
	}

#ifdef CONFIG_LOCAL_TIMERS
	for (cpu = 0; cpu < NR_CPUS; cpu++) {

		debug_printf(state, "LOC %d: %10u %11u\n", cpu,
			     __IRQ_STAT(cpu, local_timer_irqs),
			     __IRQ_STAT(cpu, local_timer_irqs) -
			     state->last_local_timer_irqs[cpu]);
		state->last_local_timer_irqs[cpu] =
			__IRQ_STAT(cpu, local_timer_irqs);
	}
#endif
}

struct stacktrace_state {
	struct fiq_debugger_state *state;
	unsigned int depth;
};

static int report_trace(struct stackframe *frame, void *d)
{
	struct stacktrace_state *sts = d;

	if (sts->depth) {
		debug_printf(sts->state,
			"  pc: %p (%pF), lr %p (%pF), sp %p, fp %p\n",
			frame->pc, frame->pc, frame->lr, frame->lr,
			frame->sp, frame->fp);
		sts->depth--;
		return 0;
	}
	debug_printf(sts->state, "  ...\n");

	return sts->depth == 0;
}

struct frame_tail {
	struct frame_tail *fp;
	unsigned long sp;
	unsigned long lr;
} __attribute__((packed));

static struct frame_tail *user_backtrace(struct fiq_debugger_state *state,
					struct frame_tail *tail)
{
	struct frame_tail buftail[2];

	/* Also check accessibility of one struct frame_tail beyond */
	if (!access_ok(VERIFY_READ, tail, sizeof(buftail))) {
		debug_printf(state, "  invalid frame pointer %p\n", tail);
		return NULL;
	}
	if (__copy_from_user_inatomic(buftail, tail, sizeof(buftail))) {
		debug_printf(state,
			"  failed to copy frame pointer %p\n", tail);
		return NULL;
	}

	debug_printf(state, "  %p\n", buftail[0].lr);

	/* frame pointers should strictly progress back up the stack
	 * (towards higher addresses) */
	if (tail >= buftail[0].fp)
		return NULL;

	return buftail[0].fp-1;
}

void dump_stacktrace(struct fiq_debugger_state *state,
		struct pt_regs * const regs, unsigned int depth, void *ssp)
{
	struct frame_tail *tail;
	struct thread_info *real_thread_info = THREAD_INFO(ssp);
	struct stacktrace_state sts;

	sts.depth = depth;
	sts.state = state;
	*current_thread_info() = *real_thread_info;

	if (!current)
		debug_printf(state, "current NULL\n");
	else
		debug_printf(state, "pid: %d  comm: %s\n",
			current->pid, current->comm);
	dump_regs(state, (unsigned *)regs);

	if (!user_mode(regs)) {
		struct stackframe frame;
		frame.fp = regs->ARM_fp;
		frame.sp = regs->ARM_sp;
		frame.lr = regs->ARM_lr;
		frame.pc = regs->ARM_pc;
		debug_printf(state,
			"  pc: %p (%pF), lr %p (%pF), sp %p, fp %p\n",
			regs->ARM_pc, regs->ARM_pc, regs->ARM_lr, regs->ARM_lr,
			regs->ARM_sp, regs->ARM_fp);
		walk_stackframe(&frame, report_trace, &sts);
		return;
	}

	tail = ((struct frame_tail *) regs->ARM_fp) - 1;
	while (depth-- && tail && !((unsigned long) tail & 3))
		tail = user_backtrace(state, tail);
}

static void do_ps(struct fiq_debugger_state *state)
{
	struct task_struct *g;
	struct task_struct *p;
	unsigned task_state;
	static const char stat_nam[] = "RSDTtZX";

	debug_printf(state, "pid   ppid  prio task            pc\n");
	read_lock(&tasklist_lock);
	do_each_thread(g, p) {
		task_state = p->state ? __ffs(p->state) + 1 : 0;
		debug_printf(state,
			     "%5d %5d %4d ", p->pid, p->parent->pid, p->prio);
		debug_printf(state, "%-13.13s %c", p->comm,
			     task_state >= sizeof(stat_nam) ? '?' : stat_nam[task_state]);
		if (task_state == TASK_RUNNING)
			debug_printf(state, " running\n");
		else
			debug_printf(state, " %08lx\n", thread_saved_pc(p));
	} while_each_thread(g, p);
	read_unlock(&tasklist_lock);
}

#ifdef CONFIG_FIQ_DEBUGGER_CONSOLE
static void begin_syslog_dump(struct fiq_debugger_state *state)
{
	state->syslog_dumping = true;
}

static void end_syslog_dump(struct fiq_debugger_state *state)
{
	state->syslog_dumping = false;
}
#else
extern int do_syslog(int type, char __user *bug, int count);
static void begin_syslog_dump(struct fiq_debugger_state *state)
{
	do_syslog(5 /* clear */, NULL, 0);
}

static void end_syslog_dump(struct fiq_debugger_state *state)
{
	char buf[128];
	int ret;
	int idx = 0;

	while (1) {
		ret = log_buf_copy(buf, idx, sizeof(buf) - 1);
		if (ret <= 0)
			break;
		buf[ret] = 0;
		debug_printf(state, "%s", buf);
		idx += ret;
	}
}
#endif

static void do_sysrq(struct fiq_debugger_state *state, char rq)
{
	begin_syslog_dump(state);
	handle_sysrq(rq);
	end_syslog_dump(state);
}

/* This function CANNOT be called in FIQ context */
static void debug_irq_exec(struct fiq_debugger_state *state, char *cmd)
{
	int invalid_cmd = 0;
	if (!strcmp(cmd, "ps"))
		do_ps(state);
	else if (!strcmp(cmd, "sysrq"))
		do_sysrq(state, 'h');
	else if (!strncmp(cmd, "sysrq ", 6))
		do_sysrq(state, cmd[6]);
	else {
		invalid_cmd = 1;
		memset(state->debug_buf, 0, DEBUG_MAX);
	}

	if (invalid_cmd == 0) {
		state->current_pointer = (state->current_pointer-1) & CMD_COUNT;
		if (strcmp(state->cmd_buf[state->current_pointer], state->debug_buf)) {
			state->current_pointer = (state->current_pointer+1) & CMD_COUNT;
			memset(state->cmd_buf[state->current_pointer], 0, DEBUG_MAX);
			strcpy(state->cmd_buf[state->current_pointer], state->debug_buf);
		}
		memset(state->debug_buf, 0, DEBUG_MAX);
		state->current_pointer = (state->current_pointer+1) & CMD_COUNT;
		state->back_pointer = state->current_pointer;
	}
}

static char cmd_buf[][16] = {
		{"pc"},
		{"regs"},
		{"allregs"},
		{"bt"},
		{"reboot"},
		{"irqs"},
		{"kmsg"},
#ifdef CONFIG_RK29_LAST_LOG
		{"last_kmsg"},
#endif
		{"version"},
		{"sleep"},
		{"nosleep"},
		{"console"},
		{"cpu"},
		{"ps"},
		{"sysrq"},
};

static void debug_help(struct fiq_debugger_state *state)
{
	debug_printf(state,	"FIQ Debugger commands:\n"
				" pc            PC status\n"
				" regs          Register dump\n"
				" allregs       Extended Register dump\n"
				" bt            Stack trace\n"
				" reboot        Reboot\n"
				" irqs          Interupt status\n"
				" kmsg          Kernel log\n"
				" version       Kernel version\n");
#ifdef CONFIG_RK29_LAST_LOG
	debug_printf(state,	" last_kmsg     Last kernel log\n");
#endif
	debug_printf(state,	" sleep         Allow sleep while in FIQ\n"
				" nosleep       Disable sleep while in FIQ\n"
				" console       Switch terminal to console\n"
				" cpu           Current CPU\n"
				" cpu <number>  Switch to CPU<number>\n");
	debug_printf(state,	" ps            Process list\n"
				" sysrq         sysrq options\n"
				" sysrq <param> Execute sysrq with <param>\n");
}

static void take_affinity(void *info)
{
	struct fiq_debugger_state *state = info;
	struct cpumask cpumask;

	cpumask_clear(&cpumask);
	cpumask_set_cpu(get_cpu(), &cpumask);

	irq_set_affinity(state->uart_irq, &cpumask);
}

static void switch_cpu(struct fiq_debugger_state *state, int cpu)
{
	if (!debug_have_fiq(state))
		smp_call_function_single(cpu, take_affinity, state, false);
#ifdef CONFIG_PLAT_RK
	else {
		struct cpumask cpumask;

		if (!cpu_online(cpu)) {
			debug_printf(state, "cpu %d offline\n", cpu);
			return;
		}

		cpumask_clear(&cpumask);
		cpumask_set_cpu(cpu, &cpumask);

		irq_set_affinity(state->fiq, &cpumask);
		irq_set_affinity(state->uart_irq, &cpumask);
	}
#endif
	state->current_cpu = cpu;
}

static bool debug_fiq_exec(struct fiq_debugger_state *state,
			const char *cmd, unsigned *regs, void *svc_sp)
{
	bool signal_helper = false;

	if (!strcmp(cmd, "help") || !strcmp(cmd, "?")) {
		debug_help(state);
	} else if (!strcmp(cmd, "pc")) {
		debug_printf(state, " pc %08x cpsr %08x mode %s\n",
			regs[15], regs[16], mode_name(regs[16]));
	} else if (!strcmp(cmd, "regs")) {
		dump_regs(state, regs);
	} else if (!strcmp(cmd, "allregs")) {
		dump_allregs(state, regs);
	} else if (!strcmp(cmd, "bt")) {
		dump_stacktrace(state, (struct pt_regs *)regs, 100, svc_sp);
	} else if (!strcmp(cmd, "reboot")) {
		arch_reset(0, 0);
	} else if (!strncmp(cmd, "reboot ", 7)) {
		arch_reset(0, &cmd[7]);
	} else if (!strcmp(cmd, "irqs")) {
		dump_irqs(state);
	} else if (!strcmp(cmd, "kmsg")) {
		dump_kernel_log(state);
#ifdef CONFIG_RK29_LAST_LOG
	} else if (!strcmp(cmd, "last_kmsg")) {
		dump_last_kernel_log(state);
#endif
	} else if (!strcmp(cmd, "version")) {
		debug_printf(state, "%s\n", linux_banner);
	} else if (!strcmp(cmd, "sleep")) {
		state->no_sleep = false;
		debug_printf(state, "enabling sleep\n");
	} else if (!strcmp(cmd, "nosleep")) {
		state->no_sleep = true;
		debug_printf(state, "disabling sleep\n");
	} else if (!strcmp(cmd, "console")) {
		state->console_enable = true;
		debug_printf(state, "console mode\n");
	} else if (!strcmp(cmd, "cpu")) {
		debug_printf(state, "cpu %d\n", state->current_cpu);
	} else if (!strncmp(cmd, "cpu ", 4)) {
		unsigned long cpu = 0;
		if (strict_strtoul(cmd + 4, 10, &cpu) == 0)
			switch_cpu(state, cpu);
		else
			debug_printf(state, "invalid cpu\n");
		debug_printf(state, "cpu %d\n", state->current_cpu);
	} else {
		if (state->debug_busy) {
			debug_printf(state,
				"command processor busy. trying to abort.\n");
			state->debug_abort = -1;
		} else {
			strcpy(state->debug_cmd, cmd);
			state->debug_busy = 1;
		}

		return true;
	}
	if (!state->console_enable)
		debug_prompt(state);

	return signal_helper;
}

static void sleep_timer_expired(unsigned long data)
{
	struct fiq_debugger_state *state = (struct fiq_debugger_state *)data;
	unsigned long flags;

	spin_lock_irqsave(&state->sleep_timer_lock, flags);
	if (state->uart_enabled && !state->no_sleep) {
		if (state->debug_enable && !state->console_enable) {
			state->debug_enable = false;
			debug_printf_nfiq(state, "suspending fiq debugger\n");
		}
		state->ignore_next_wakeup_irq = true;
		debug_uart_disable(state);
		state->uart_enabled = false;
		enable_wakeup_irq(state);
	}
	wake_unlock(&state->debugger_wake_lock);
	spin_unlock_irqrestore(&state->sleep_timer_lock, flags);
}

static void handle_wakeup(struct fiq_debugger_state *state)
{
	unsigned long flags;

	spin_lock_irqsave(&state->sleep_timer_lock, flags);
	if (state->wakeup_irq >= 0 && state->ignore_next_wakeup_irq) {
		state->ignore_next_wakeup_irq = false;
	} else if (!state->uart_enabled) {
		wake_lock(&state->debugger_wake_lock);
		debug_uart_enable(state);
		state->uart_enabled = true;
		disable_wakeup_irq(state);
		mod_timer(&state->sleep_timer, jiffies + HZ / 2);
	}
	spin_unlock_irqrestore(&state->sleep_timer_lock, flags);
}

static irqreturn_t wakeup_irq_handler(int irq, void *dev)
{
	struct fiq_debugger_state *state = dev;

	if (!state->no_sleep)
		debug_puts(state, "WAKEUP\n");
	handle_wakeup(state);

	return IRQ_HANDLED;
}


static void debug_handle_irq_context(struct fiq_debugger_state *state)
{
	if (!state->no_sleep) {
		unsigned long flags;

		spin_lock_irqsave(&state->sleep_timer_lock, flags);
		wake_lock(&state->debugger_wake_lock);
		mod_timer(&state->sleep_timer, jiffies + HZ * 5);
		spin_unlock_irqrestore(&state->sleep_timer_lock, flags);
	}
#if defined(CONFIG_FIQ_DEBUGGER_CONSOLE)
	if (state->tty) {
		int i;
		int count = fiq_debugger_ringbuf_level(state->tty_rbuf);
		for (i = 0; i < count; i++) {
			int c = fiq_debugger_ringbuf_peek(state->tty_rbuf, 0);
			tty_insert_flip_char(state->tty, c, TTY_NORMAL);
			if (!fiq_debugger_ringbuf_consume(state->tty_rbuf, 1))
				pr_warn("fiq tty failed to consume byte\n");
		}
		tty_flip_buffer_push(state->tty);
	}
#endif
	if (state->debug_busy) {
		debug_irq_exec(state, state->debug_cmd);
		debug_prompt(state);
		state->debug_busy = 0;
	}
}

static int debug_getc(struct fiq_debugger_state *state)
{
	return state->pdata->uart_getc(state->pdev);
}


static int debug_cmd_check_back(struct fiq_debugger_state *state, char c)
{
	char *s;
	int i = 0;
	if (c == 'A') {
		state->back_pointer = (state->back_pointer-1) & CMD_COUNT;
		if (state->back_pointer != state->current_pointer) {
			s = state->cmd_buf[state->back_pointer];
			if (*s != 0) {
				for(i = 0; i < strlen(state->debug_buf)-1; i++) {
					state->pdata->uart_putc(state->pdev, 8);
					state->pdata->uart_putc(state->pdev, ' ');
					state->pdata->uart_putc(state->pdev, 8);
				}
				memset(state->debug_buf, 0, DEBUG_MAX);
				strcpy(state->debug_buf, s);
				state->debug_count = strlen(state->debug_buf);
				debug_printf(state, state->debug_buf);
			} else {
				state->back_pointer = (state->back_pointer+1) & CMD_COUNT;
			}

		} else {
			state->back_pointer = (state->back_pointer+1) & CMD_COUNT;
		}
	} else if (c == 'B') {

		if (state->back_pointer != state->current_pointer) {
			state->back_pointer = (state->back_pointer+1) & CMD_COUNT;
			if(state->back_pointer == state->current_pointer){
				goto cmd_clear;
			} else {
				s = state->cmd_buf[state->back_pointer];
				if (*s != 0) {
					for(i = 0; i < strlen(state->debug_buf)-1; i++) {
						state->pdata->uart_putc(state->pdev, 8);
						state->pdata->uart_putc(state->pdev, ' ');
						state->pdata->uart_putc(state->pdev, 8);
					}
					memset(state->debug_buf, 0, DEBUG_MAX);
					strcpy(state->debug_buf, s);
					state->debug_count = strlen(state->debug_buf);
					debug_printf(state, state->debug_buf);
				}
			}
		} else {
cmd_clear:
			for(i = 0; i < strlen(state->debug_buf)-1; i++) {
				state->pdata->uart_putc(state->pdev, 8);
				state->pdata->uart_putc(state->pdev, ' ');
				state->pdata->uart_putc(state->pdev, 8);
			}
			memset(state->debug_buf, 0, DEBUG_MAX);
			state->debug_count = 0;
		}
	}
	return 0;
}

static void debug_cmd_tab(struct fiq_debugger_state *state)
{
	int i,j;
	int count = 0;

	for (i = 0; i < ARRAY_SIZE(cmd_buf); i++) {
		cmd_buf[i][15] = 1;
	}

	for (j = 1; j <= strlen(state->debug_buf); j++) {
		count = 0;
		for (i = 0; i < ARRAY_SIZE(cmd_buf); i++) {
			if (cmd_buf[i][15] == 1) {
				if (strncmp(state->debug_buf, cmd_buf[i], j)) {
					cmd_buf[i][15] = 0;
				} else {
					count++;
				}
			}
		}
		if (count == 0)
			break;
	}

	if (count == 1) {
		for (i = 0; i < ARRAY_SIZE(cmd_buf); i++) {
			if (cmd_buf[i][15] == 1)
				break;
		}

		for(j = 0; j < strlen(state->debug_buf); j++) {
			state->pdata->uart_putc(state->pdev, 8);
			state->pdata->uart_putc(state->pdev, ' ');
			state->pdata->uart_putc(state->pdev, 8);
		}
		memset(state->debug_buf, 0, DEBUG_MAX);
		strcpy(state->debug_buf, cmd_buf[i]);
		state->debug_count = strlen(state->debug_buf);
		debug_printf(state, state->debug_buf);

	}
}

static bool debug_handle_uart_interrupt(struct fiq_debugger_state *state,
			int this_cpu, void *regs, void *svc_sp)
{
	int c;
	static int last_c;
	int count = 0;
	bool signal_helper = false;

	if (this_cpu != state->current_cpu) {
		if (state->in_fiq)
			return false;

		if (atomic_inc_return(&state->unhandled_fiq_count) !=
					MAX_UNHANDLED_FIQ_COUNT)
			return false;

		debug_printf(state, "fiq_debugger: cpu %d not responding, "
			"reverting to cpu %d\n", state->current_cpu,
			this_cpu);

		atomic_set(&state->unhandled_fiq_count, 0);
		switch_cpu(state, this_cpu);
		return false;
	}

	state->in_fiq = true;

	while ((c = debug_getc(state)) != FIQ_DEBUGGER_NO_CHAR) {
		count++;
		if (!state->debug_enable) {
			if ((c == 13) || (c == 10)) {
				state->debug_enable = true;
				state->debug_count = 0;
				debug_prompt(state);
			}
		} else if (c == FIQ_DEBUGGER_BREAK) {
			state->console_enable = false;
			debug_puts(state, "fiq debugger mode\n");
			state->debug_count = 0;
			state->back_pointer = CMD_COUNT;
			state->current_pointer = CMD_COUNT;
			memset(state->cmd_buf, 0, (CMD_COUNT+1)*DEBUG_MAX);
			debug_prompt(state);
#ifdef CONFIG_FIQ_DEBUGGER_CONSOLE
		} else if (state->console_enable && state->tty_rbuf) {
			fiq_debugger_ringbuf_push(state->tty_rbuf, c);
			signal_helper = true;
#endif
		} else if (last_c == '[' && (c == 'A' || c == 'B' || c == 'C' || c == 'D')) {
			if (state->debug_count > 0) {
				state->debug_count--;
				state->pdata->uart_putc(state->pdev, 8);
				state->pdata->uart_putc(state->pdev, ' ');
				state->pdata->uart_putc(state->pdev, 8);
			}
			debug_cmd_check_back(state, c);
			//tab
		} else if (c == 9) {
			debug_cmd_tab(state);
		} else if ((c >= ' ') && (c < 127)) {
			if (state->debug_count < (DEBUG_MAX - 1)) {
				state->debug_buf[state->debug_count++] = c;
				state->pdata->uart_putc(state->pdev, c);
			}
		} else if ((c == 8) || (c == 127)) {
			if (state->debug_count > 0) {
				state->debug_count--;
				state->pdata->uart_putc(state->pdev, 8);
				state->pdata->uart_putc(state->pdev, ' ');
				state->pdata->uart_putc(state->pdev, 8);
				state->debug_buf[state->debug_count] = 0;
			}
		} else if ((c == 13) || (c == 10)) {
			if (c == '\r' || (c == '\n' && last_c != '\r')) {
				state->pdata->uart_putc(state->pdev, '\r');
				state->pdata->uart_putc(state->pdev, '\n');
			}
			if (state->debug_count) {
				state->debug_buf[state->debug_count] = 0;
				state->debug_count = 0;
				signal_helper |=
					debug_fiq_exec(state, state->debug_buf,
						       regs, svc_sp);
				if (signal_helper == false) {
					state->current_pointer = (state->current_pointer-1) & CMD_COUNT;
					if (strcmp(state->cmd_buf[state->current_pointer], state->debug_buf)) {
						state->current_pointer = (state->current_pointer+1) & CMD_COUNT;
						memset(state->cmd_buf[state->current_pointer], 0, DEBUG_MAX);
						strcpy(state->cmd_buf[state->current_pointer], state->debug_buf);
					}
					memset(state->debug_buf, 0, DEBUG_MAX);
					state->current_pointer = (state->current_pointer+1) & CMD_COUNT;
					state->back_pointer = state->current_pointer;
				}
			} else {
				debug_prompt(state);
			}
		}
		last_c = c;
	}
	debug_uart_flush(state);
	if (state->pdata->fiq_ack)
		state->pdata->fiq_ack(state->pdev, state->fiq);

	/* poke sleep timer if necessary */
	if (state->debug_enable && !state->no_sleep)
		signal_helper = true;

	atomic_set(&state->unhandled_fiq_count, 0);
	state->in_fiq = false;

	return signal_helper;
}

static void debug_fiq(struct fiq_glue_handler *h, void *regs, void *svc_sp)
{
	struct fiq_debugger_state *state =
		container_of(h, struct fiq_debugger_state, handler);
	unsigned int this_cpu = THREAD_INFO(svc_sp)->cpu;
	bool need_irq;

	/* RK2928 USB-UART function, otg dp/dm default in uart status;
	 * connect with otg cable&usb device, dp/dm will be hi-z status 
	 * and make uart controller enter infinite fiq loop 
	 */
#ifdef CONFIG_RK_USB_UART
#ifdef CONFIG_ARCH_RK2928
	if(!(readl_relaxed(RK2928_GRF_BASE + 0x014c) & (1<<10))){//id low          
		writel_relaxed(0x34000000, RK2928_GRF_BASE + 0x190);   //enter usb phy    
	}
#elif defined(CONFIG_ARCH_RK3188)
	if(!(readl_relaxed(RK30_GRF_BASE + 0x00ac) & (1 << 13))){//id low          
		writel_relaxed((0x0300 << 16), RK30_GRF_BASE + 0x010c);   //enter usb phy    
	}
#endif
#endif
	need_irq = debug_handle_uart_interrupt(state, this_cpu, regs, svc_sp);
	if (need_irq)
		debug_force_irq(state);
}

/*
 * When not using FIQs, we only use this single interrupt as an entry point.
 * This just effectively takes over the UART interrupt and does all the work
 * in this context.
 */
static irqreturn_t debug_uart_irq(int irq, void *dev)
{
	struct fiq_debugger_state *state = dev;
	bool not_done;

	handle_wakeup(state);

	/* handle the debugger irq in regular context */
	not_done = debug_handle_uart_interrupt(state, smp_processor_id(),
					      get_irq_regs(),
					      current_thread_info());
	if (not_done)
		debug_handle_irq_context(state);

	return IRQ_HANDLED;
}

/*
 * If FIQs are used, not everything can happen in fiq context.
 * FIQ handler does what it can and then signals this interrupt to finish the
 * job in irq context.
 */
static irqreturn_t debug_signal_irq(int irq, void *dev)
{
	struct fiq_debugger_state *state = dev;

	if (state->pdata->force_irq_ack)
		state->pdata->force_irq_ack(state->pdev, state->signal_irq);

	debug_handle_irq_context(state);

	return IRQ_HANDLED;
}

static void debug_resume(struct fiq_glue_handler *h)
{
	struct fiq_debugger_state *state =
		container_of(h, struct fiq_debugger_state, handler);
	if (state->pdata->uart_resume)
		state->pdata->uart_resume(state->pdev);
}

#if defined(CONFIG_FIQ_DEBUGGER_CONSOLE)
struct tty_driver *debug_console_device(struct console *co, int *index)
{
	struct fiq_debugger_state *state;
	state = container_of(co, struct fiq_debugger_state, console);
	*index = 0;
	return state->tty_driver;
}

static void debug_console_write(struct console *co,
				const char *s, unsigned int count)
{
	struct fiq_debugger_state *state;

	state = container_of(co, struct fiq_debugger_state, console);

	if (!state->console_enable && !state->syslog_dumping)
		return;

#ifdef CONFIG_RK_CONSOLE_THREAD
	if (state->pdata->console_write) {
		state->pdata->console_write(state->pdev, s, count);
		return;
	}
#endif

	debug_uart_enable(state);
	while (count--) {
		if (*s == '\n')
			state->pdata->uart_putc(state->pdev, '\r');
		state->pdata->uart_putc(state->pdev, *s++);
	}
	debug_uart_flush(state);
	debug_uart_disable(state);
}

static struct console fiq_debugger_console = {
	.name = "ttyFIQ",
	.device = debug_console_device,
	.write = debug_console_write,
	.flags = CON_PRINTBUFFER | CON_ANYTIME | CON_ENABLED,
};

int fiq_tty_open(struct tty_struct *tty, struct file *filp)
{
	struct fiq_debugger_state *state = tty->driver->driver_state;
	if (state->tty_open_count++)
		return 0;

	tty->driver_data = state;
	state->tty = tty;
	return 0;
}

void fiq_tty_close(struct tty_struct *tty, struct file *filp)
{
	struct fiq_debugger_state *state = tty->driver_data;
	if (--state->tty_open_count)
		return;
	state->tty = NULL;
}

int  fiq_tty_write(struct tty_struct *tty, const unsigned char *buf, int count)
{
	int i;
	struct fiq_debugger_state *state = tty->driver_data;

	if (!state->console_enable)
		return count;

	debug_uart_enable(state);
	for (i = 0; i < count; i++)
		state->pdata->uart_putc(state->pdev, *buf++);
	debug_uart_disable(state);

	return count;
}

int  fiq_tty_write_room(struct tty_struct *tty)
{
	return 1024;
}

static const struct tty_operations fiq_tty_driver_ops = {
	.write = fiq_tty_write,
	.write_room = fiq_tty_write_room,
	.open = fiq_tty_open,
	.close = fiq_tty_close,
};

static int fiq_debugger_tty_init(struct fiq_debugger_state *state)
{
	int ret = -EINVAL;

	state->tty_driver = alloc_tty_driver(1);
	if (!state->tty_driver) {
		pr_err("Failed to allocate fiq debugger tty\n");
		return -ENOMEM;
	}

	state->tty_driver->owner		= THIS_MODULE;
	state->tty_driver->driver_name	= "fiq-debugger";
	state->tty_driver->name		= "ttyFIQ";
	state->tty_driver->type		= TTY_DRIVER_TYPE_SERIAL;
	state->tty_driver->subtype	= SERIAL_TYPE_NORMAL;
	state->tty_driver->init_termios	= tty_std_termios;
	state->tty_driver->init_termios.c_cflag =
					B115200 | CS8 | CREAD | HUPCL | CLOCAL;
	state->tty_driver->init_termios.c_ispeed =
		state->tty_driver->init_termios.c_ospeed = 115200;
	state->tty_driver->flags		= TTY_DRIVER_REAL_RAW;
	tty_set_operations(state->tty_driver, &fiq_tty_driver_ops);
	state->tty_driver->driver_state = state;

	ret = tty_register_driver(state->tty_driver);
	if (ret) {
		pr_err("Failed to register fiq tty: %d\n", ret);
		goto err;
	}

	state->tty_rbuf = fiq_debugger_ringbuf_alloc(1024);
	if (!state->tty_rbuf) {
		pr_err("Failed to allocate fiq debugger ringbuf\n");
		ret = -ENOMEM;
		goto err;
	}

	pr_info("Registered FIQ tty driver %p\n", state->tty_driver);
	return 0;

err:
	fiq_debugger_ringbuf_free(state->tty_rbuf);
	state->tty_rbuf = NULL;
	put_tty_driver(state->tty_driver);
	return ret;
}
#endif

static int fiq_debugger_dev_suspend(struct device *dev)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct fiq_debugger_state *state = platform_get_drvdata(pdev);

	if (state->pdata->uart_dev_suspend)
		return state->pdata->uart_dev_suspend(pdev);
	return 0;
}

static int fiq_debugger_dev_resume(struct device *dev)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct fiq_debugger_state *state = platform_get_drvdata(pdev);

	if (state->pdata->uart_dev_resume)
		return state->pdata->uart_dev_resume(pdev);
	return 0;
}

static int fiq_debugger_probe(struct platform_device *pdev)
{
	int ret;
	struct fiq_debugger_pdata *pdata = dev_get_platdata(&pdev->dev);
	struct fiq_debugger_state *state;
	int fiq;
	int uart_irq;

	if (!pdata->uart_getc || !pdata->uart_putc)
		return -EINVAL;
	if ((pdata->uart_enable && !pdata->uart_disable) ||
	    (!pdata->uart_enable && pdata->uart_disable))
		return -EINVAL;

	fiq = platform_get_irq_byname(pdev, "fiq");
	uart_irq = platform_get_irq_byname(pdev, "uart_irq");

	/* uart_irq mode and fiq mode are mutually exclusive, but one of them
	 * is required */
	if ((uart_irq < 0 && fiq < 0) || (uart_irq >= 0 && fiq >= 0))
		return -EINVAL;
	if (fiq >= 0 && !pdata->fiq_enable)
		return -EINVAL;

	state = kzalloc(sizeof(*state), GFP_KERNEL);
	setup_timer(&state->sleep_timer, sleep_timer_expired,
		    (unsigned long)state);
	state->pdata = pdata;
	state->pdev = pdev;
	state->no_sleep = initial_no_sleep;
	state->debug_enable = initial_debug_enable;
	state->console_enable = initial_console_enable;

	state->fiq = fiq;
	state->uart_irq = uart_irq;
	state->signal_irq = platform_get_irq_byname(pdev, "signal");
	state->wakeup_irq = platform_get_irq_byname(pdev, "wakeup");

	platform_set_drvdata(pdev, state);

	spin_lock_init(&state->sleep_timer_lock);

	if (state->wakeup_irq < 0 && debug_have_fiq(state))
		state->no_sleep = true;
	state->ignore_next_wakeup_irq = !state->no_sleep;

	wake_lock_init(&state->debugger_wake_lock,
			WAKE_LOCK_SUSPEND, "serial-debug");

	state->clk = clk_get(&pdev->dev, NULL);
	if (IS_ERR(state->clk))
		state->clk = NULL;

	/* do not call pdata->uart_enable here since uart_init may still
	 * need to do some initialization before uart_enable can work.
	 * So, only try to manage the clock during init.
	 */
	if (state->clk)
		clk_enable(state->clk);

	if (pdata->uart_init) {
		ret = pdata->uart_init(pdev);
		if (ret)
			goto err_uart_init;
	}

	debug_printf_nfiq(state, "<hit enter %sto activate fiq debugger>\n",
				state->no_sleep ? "" : "twice ");

	if (debug_have_fiq(state)) {
		state->handler.fiq = debug_fiq;
		state->handler.resume = debug_resume;
		ret = fiq_glue_register_handler(&state->handler);
		if (ret) {
			pr_err("%s: could not install fiq handler\n", __func__);
			goto err_register_fiq;
		}
		//close uart fiq
		//pdata->fiq_enable(pdev, state->fiq, 0);
	} else {
		ret = request_irq(state->uart_irq, debug_uart_irq,
				  IRQF_NO_SUSPEND, "debug", state);
		if (ret) {
			pr_err("%s: could not install irq handler\n", __func__);
			goto err_register_irq;
		}

		/* for irq-only mode, we want this irq to wake us up, if it
		 * can.
		 */
		enable_irq_wake(state->uart_irq);
	}

	if (state->clk)
		clk_disable(state->clk);

	if (state->signal_irq >= 0) {
		ret = request_irq(state->signal_irq, debug_signal_irq,
			  IRQF_TRIGGER_RISING, "debug-signal", state);
		if (ret)
			pr_err("serial_debugger: could not install signal_irq");
	}

	if (state->wakeup_irq >= 0) {
		ret = request_irq(state->wakeup_irq, wakeup_irq_handler,
				  IRQF_TRIGGER_FALLING | IRQF_DISABLED,
				  "debug-wakeup", state);
		if (ret) {
			pr_err("serial_debugger: "
				"could not install wakeup irq\n");
			state->wakeup_irq = -1;
		} else {
			ret = enable_irq_wake(state->wakeup_irq);
			if (ret) {
				pr_err("serial_debugger: "
					"could not enable wakeup\n");
				state->wakeup_irq_no_set_wake = true;
			}
		}
	}
	if (state->no_sleep)
		handle_wakeup(state);

#if defined(CONFIG_FIQ_DEBUGGER_CONSOLE)
	state->console = fiq_debugger_console;
	register_console(&state->console);
	fiq_debugger_tty_init(state);
#endif
	return 0;

err_register_irq:
err_register_fiq:
	if (pdata->uart_free)
		pdata->uart_free(pdev);
err_uart_init:
	if (state->clk)
		clk_disable(state->clk);
	if (state->clk)
		clk_put(state->clk);
	wake_lock_destroy(&state->debugger_wake_lock);
	platform_set_drvdata(pdev, NULL);
	kfree(state);
	return ret;
}

static const struct dev_pm_ops fiq_debugger_dev_pm_ops = {
	.suspend	= fiq_debugger_dev_suspend,
	.resume		= fiq_debugger_dev_resume,
};

static struct platform_driver fiq_debugger_driver = {
	.probe	= fiq_debugger_probe,
	.driver	= {
		.name	= "fiq_debugger",
		.pm	= &fiq_debugger_dev_pm_ops,
	},
};

static int __init fiq_debugger_init(void)
{
	return platform_driver_register(&fiq_debugger_driver);
}

postcore_initcall(fiq_debugger_init);
