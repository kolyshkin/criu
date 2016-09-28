#include <unistd.h>
#include <inttypes.h>

#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/mman.h>

#include "protobuf.h"
#include "images/sa.pb-c.h"
#include "images/timer.pb-c.h"
#include "images/creds.pb-c.h"
#include "images/core.pb-c.h"
#include "images/pagemap.pb-c.h"

#include "imgset.h"
#include "ptrace.h"
#include "asm/processor-flags.h"
#include "parasite-syscall.h"
#include "parasite-blob.h"
#include "parasite.h"
#include "crtools.h"
#include "namespaces.h"
#include "kerndat.h"
#include "config.h"
#include "pstree.h"
#include "posix-timer.h"
#include "mem.h"
#include "vma.h"
#include "proc_parse.h"
#include "aio.h"
#include "fault-injection.h"
#include "uapi/std/syscall-codes.h"
#include "signal.h"

#include <string.h>
#include <stdlib.h>
#include <elf.h>

#include "asm/parasite-syscall.h"
#include "asm/dump.h"
#include "asm/restorer.h"
#include "pie/pie-relocs.h"

#include "infect.h"
#include "infect-priv.h"

#define MEMFD_FNAME	"CRIUMFD"
#define MEMFD_FNAME_SZ	sizeof(MEMFD_FNAME)

unsigned long get_exec_start(struct vm_area_list *vmas)
{
	struct vma_area *vma_area;

	list_for_each_entry(vma_area, &vmas->h, list) {
		unsigned long len;

		if (vma_area->e->start >= kdat.task_size)
			continue;
		if (!(vma_area->e->prot & PROT_EXEC))
			continue;

		len = vma_area_len(vma_area);
		if (len < BUILTIN_SYSCALL_SIZE + MEMFD_FNAME_SZ) {
			pr_warn("Suspiciously short VMA @%#lx\n", (unsigned long)vma_area->e->start);
			continue;
		}

		return vma_area->e->start;
	}

	return 0;
}

#ifndef ARCH_HAS_GET_REGS
static inline int ptrace_get_regs(int pid, user_regs_struct_t *regs)
{
	struct iovec iov;

	iov.iov_base = regs;
	iov.iov_len = sizeof(user_regs_struct_t);
	return ptrace(PTRACE_GETREGSET, pid, NT_PRSTATUS, &iov);
}

static inline int ptrace_set_regs(int pid, user_regs_struct_t *regs)
{
	struct iovec iov;

	iov.iov_base = regs;
	iov.iov_len = sizeof(user_regs_struct_t);
	return ptrace(PTRACE_SETREGSET, pid, NT_PRSTATUS, &iov);
}
#endif

bool seized_native(struct parasite_ctl *ctl)
{
	return user_regs_native(&ctl->orig.regs);
}

static int __parasite_send_cmd(int sockfd, struct ctl_msg *m)
{
	int ret;

	ret = send(sockfd, m, sizeof(*m), 0);
	if (ret == -1) {
		pr_perror("Failed to send command %d to daemon", m->cmd);
		return -1;
	} else if (ret != sizeof(*m)) {
		pr_err("Message to daemon is trimmed (%d/%d)\n",
		       (int)sizeof(*m), ret);
		return -1;
	}

	pr_debug("Sent msg to daemon %d %d %d\n", m->cmd, m->ack, m->err);
	return 0;
}

int parasite_wait_ack(int sockfd, unsigned int cmd, struct ctl_msg *m)
{
	int ret;

	pr_debug("Wait for ack %d on daemon socket\n", cmd);

	while (1) {
		memzero(m, sizeof(*m));

		ret = recv(sockfd, m, sizeof(*m), MSG_WAITALL);
		if (ret == -1) {
			pr_perror("Failed to read ack");
			return -1;
		} else if (ret != sizeof(*m)) {
			pr_err("Message reply from daemon is trimmed (%d/%d)\n",
			       (int)sizeof(*m), ret);
			return -1;
		}
		pr_debug("Fetched ack: %d %d %d\n",
			 m->cmd, m->ack, m->err);

		if (m->cmd != cmd || m->ack != cmd) {
			pr_err("Communication error, this is not "
			       "the ack we expected\n");
			return -1;
		}
		return 0;
	}

	return -1;
}

int __parasite_wait_daemon_ack(unsigned int cmd,
					struct parasite_ctl *ctl)
{
	struct ctl_msg m;

	if (parasite_wait_ack(ctl->tsock, cmd, &m))
		return -1;

	if (m.err != 0) {
		pr_err("Command %d for daemon failed with %d\n",
		       cmd, m.err);
		return -1;
	}

	return 0;
}

int __parasite_execute_daemon(unsigned int cmd, struct parasite_ctl *ctl)
{
	struct ctl_msg m;

	m = ctl_msg_cmd(cmd);
	return __parasite_send_cmd(ctl->tsock, &m);
}

int parasite_execute_daemon(unsigned int cmd, struct parasite_ctl *ctl)
{
	int ret;

	ret = __parasite_execute_daemon(cmd, ctl);
	if (!ret)
		ret = __parasite_wait_daemon_ack(cmd, ctl);

	return ret;
}

int parasite_send_fd(struct parasite_ctl *ctl, int fd)
{
	if (send_fd(ctl->tsock, NULL, 0, fd) < 0) {
		pr_perror("Can't send file descriptor");
		return -1;
	}
	return 0;
}

/*
 * We need to detect parasite crashes not to hang on socket operations.
 * Since CRIU holds parasite with ptrace, it will receive SIGCHLD if the
 * latter would crash.
 *
 * This puts a restriction on how to execute a sub-process on dump stage.
 * One should use the cr_system helper, that blocks sigcild and waits
 * for the spawned program to finish.
 */
static void sigchld_handler(int signal, siginfo_t *siginfo, void *data)
{
	int pid, status;

	pid = waitpid(-1, &status, WNOHANG);
	if (pid <= 0)
		return;

	pr_err("si_code=%d si_pid=%d si_status=%d\n",
		siginfo->si_code, siginfo->si_pid, siginfo->si_status);

	if (WIFEXITED(status))
		pr_err("%d exited with %d unexpectedly\n", pid, WEXITSTATUS(status));
	else if (WIFSIGNALED(status))
		pr_err("%d was killed by %d unexpectedly: %s\n",
			pid, WTERMSIG(status), strsignal(WTERMSIG(status)));
	else if (WIFSTOPPED(status))
		pr_err("%d was stopped by %d unexpectedly\n", pid, WSTOPSIG(status));

	exit(1);
}

static int alloc_groups_copy_creds(CredsEntry *ce, struct parasite_dump_creds *c)
{
	BUILD_BUG_ON(sizeof(ce->groups[0]) != sizeof(c->groups[0]));
	BUILD_BUG_ON(sizeof(ce->cap_inh[0]) != sizeof(c->cap_inh[0]));
	BUILD_BUG_ON(sizeof(ce->cap_prm[0]) != sizeof(c->cap_prm[0]));
	BUILD_BUG_ON(sizeof(ce->cap_eff[0]) != sizeof(c->cap_eff[0]));
	BUILD_BUG_ON(sizeof(ce->cap_bnd[0]) != sizeof(c->cap_bnd[0]));

	BUG_ON(ce->n_cap_inh != CR_CAP_SIZE);
	BUG_ON(ce->n_cap_prm != CR_CAP_SIZE);
	BUG_ON(ce->n_cap_eff != CR_CAP_SIZE);
	BUG_ON(ce->n_cap_bnd != CR_CAP_SIZE);

	memcpy(ce->cap_inh, c->cap_inh, sizeof(c->cap_inh[0]) * CR_CAP_SIZE);
	memcpy(ce->cap_prm, c->cap_prm, sizeof(c->cap_prm[0]) * CR_CAP_SIZE);
	memcpy(ce->cap_eff, c->cap_eff, sizeof(c->cap_eff[0]) * CR_CAP_SIZE);
	memcpy(ce->cap_bnd, c->cap_bnd, sizeof(c->cap_bnd[0]) * CR_CAP_SIZE);

	ce->secbits	= c->secbits;
	ce->n_groups	= c->ngroups;

	ce->groups	= xmemdup(c->groups, sizeof(c->groups[0]) * c->ngroups);

	ce->uid		= c->uids[0];
	ce->gid		= c->gids[0];
	ce->euid	= c->uids[1];
	ce->egid	= c->gids[1];
	ce->suid	= c->uids[2];
	ce->sgid	= c->gids[2];
	ce->fsuid	= c->uids[3];
	ce->fsgid	= c->gids[3];

	return ce->groups ? 0 : -ENOMEM;
}

int parasite_dump_thread_leader_seized(struct parasite_ctl *ctl, int pid, CoreEntry *core)
{
	ThreadCoreEntry *tc = core->thread_core;
	struct parasite_dump_thread *args = NULL;
	struct parasite_dump_thread_compat *args_c = NULL;
	struct parasite_dump_creds *pc;
	int ret;

	if (seized_native(ctl)) {
		args = compel_parasite_args(ctl, struct parasite_dump_thread);
		pc = args->creds;
	} else {
		args_c = compel_parasite_args(ctl, struct parasite_dump_thread_compat);
		pc = args_c->creds;
	}

	pc->cap_last_cap = kdat.last_cap;

	ret = parasite_execute_daemon(PARASITE_CMD_DUMP_THREAD, ctl);
	if (ret < 0)
		return ret;

	ret = alloc_groups_copy_creds(tc->creds, pc);
	if (ret) {
		pr_err("Can't copy creds for thread leader %d\n", pid);
		return -1;
	}

	if (seized_native(ctl))
		return dump_thread_core(pid, core, true, args);
	else
		return dump_thread_core(pid, core, false, args_c);
}

int parasite_dump_thread_seized(struct parasite_ctl *ctl, int id,
				struct pid *tid, CoreEntry *core)
{
	struct parasite_dump_thread *args = NULL;
	struct parasite_dump_thread_compat *args_c = NULL;
	pid_t pid = tid->real;
	ThreadCoreEntry *tc = core->thread_core;
	CredsEntry *creds = tc->creds;
	struct parasite_dump_creds *pc;
	int ret;
	struct thread_ctx octx;

	BUG_ON(id == 0); /* Leader is dumped in dump_task_core_all */

	if (seized_native(ctl)) {
		args = compel_parasite_args(ctl, struct parasite_dump_thread);
		pc = args->creds;
	} else {
		args_c = compel_parasite_args(ctl, struct parasite_dump_thread_compat);
		pc = args_c->creds;
	}

	pc->cap_last_cap = kdat.last_cap;

	ret = compel_prepare_thread(pid, &octx);
	if (ret)
		return -1;

	tc->has_blk_sigset = true;
	memcpy(&tc->blk_sigset, &octx.sigmask, sizeof(k_rtsigset_t));

	ret = compel_run_in_thread(pid, PARASITE_CMD_DUMP_THREAD, ctl, &octx);
	if (ret) {
		pr_err("Can't init thread in parasite %d\n", pid);
		return -1;
	}

	ret = alloc_groups_copy_creds(creds, pc);
	if (ret) {
		pr_err("Can't copy creds for thread %d\n", pid);
		return -1;
	}

	ret = get_task_regs(pid, octx.regs, save_task_regs, core);
	if (ret) {
		pr_err("Can't obtain regs for thread %d\n", pid);
		return -1;
	}

	if (seized_native(ctl)) {
		tid->virt = args->tid;
		return dump_thread_core(pid, core, true, args);
	} else {
		tid->virt = args_c->tid;
		return dump_thread_core(pid, core, false, args_c);
	}
}

#define ASSIGN_SAS(se, args)							\
do {										\
	ASSIGN_TYPED(se.sigaction, encode_pointer(				\
				(void*)(uintptr_t)args->sas[i].rt_sa_handler));	\
	ASSIGN_TYPED(se.flags, args->sas[i].rt_sa_flags);			\
	ASSIGN_TYPED(se.restorer, encode_pointer(				\
				(void*)(uintptr_t)args->sas[i].rt_sa_restorer));\
	BUILD_BUG_ON(sizeof(se.mask) != sizeof(args->sas[0].rt_sa_mask.sig));	\
	memcpy(&se.mask, args->sas[i].rt_sa_mask.sig, sizeof(se.mask));		\
} while(0)
int parasite_dump_sigacts_seized(struct parasite_ctl *ctl, struct cr_imgset *cr_imgset)
{
	struct parasite_dump_sa_args *args = NULL;
	struct parasite_dump_sa_args_compat *args_c = NULL;
	int ret, sig;
	struct cr_img *img;
	SaEntry se = SA_ENTRY__INIT;
	bool native_task = seized_native(ctl);

	if (native_task)
		args = compel_parasite_args(ctl, struct parasite_dump_sa_args);
	else
		args_c = compel_parasite_args(ctl, struct parasite_dump_sa_args_compat);

	ret = parasite_execute_daemon(PARASITE_CMD_DUMP_SIGACTS, ctl);
	if (ret < 0)
		return ret;

	img = img_from_set(cr_imgset, CR_FD_SIGACT);

	for (sig = 1; sig <= SIGMAX; sig++) {
		int i = sig - 1;

		if (sig == SIGSTOP || sig == SIGKILL)
			continue;

		if (native_task)
			ASSIGN_SAS(se, args);
		else
			ASSIGN_SAS(se, args_c);
		se.has_compat_sigaction = true;
		se.compat_sigaction = !native_task;

		if (pb_write_one(img, &se, PB_SIGACT) < 0)
			return -1;
	}

	return 0;
}

#define encode_itimer(v, ie)							\
do {										\
	ie->isec = v->it_interval.tv_sec;					\
	ie->iusec = v->it_interval.tv_usec;					\
	ie->vsec = v->it_value.tv_sec;						\
	ie->vusec = v->it_value.tv_usec;					\
} while(0)									\

#define ASSIGN_ITIMER(args)							\
do {										\
	encode_itimer((&args->real), (core->tc->timers->real));			\
	encode_itimer((&args->virt), (core->tc->timers->virt));			\
	encode_itimer((&args->prof), (core->tc->timers->prof));			\
} while(0)
int parasite_dump_itimers_seized(struct parasite_ctl *ctl, struct pstree_item *item)
{
	CoreEntry *core = item->core[0];
	struct parasite_dump_itimers_args *args = NULL;
	struct parasite_dump_itimers_args_compat *args_c = NULL;
	int ret;

	if (seized_native(ctl))
		args = compel_parasite_args(ctl, struct parasite_dump_itimers_args);
	else
		args_c = compel_parasite_args(ctl, struct parasite_dump_itimers_args_compat);

	ret = parasite_execute_daemon(PARASITE_CMD_DUMP_ITIMERS, ctl);
	if (ret < 0)
		return ret;

	if (seized_native(ctl))
		ASSIGN_ITIMER(args);
	else
		ASSIGN_ITIMER(args_c);

	return 0;
}

static int core_alloc_posix_timers(TaskTimersEntry *tte, int n,
		PosixTimerEntry **pte)
{
	int sz;

	/*
	 * Will be free()-ed in core_entry_free()
	 */

	sz = n * (sizeof(PosixTimerEntry *) + sizeof(PosixTimerEntry));
	tte->posix = xmalloc(sz);
	if (!tte->posix)
		return -1;

	tte->n_posix = n;
	*pte = (PosixTimerEntry *)(tte->posix + n);
	return 0;
}

#define set_posix_timer_arg(args, ctl, m, val)					\
do {										\
	if (seized_native(ctl))							\
		ASSIGN_TYPED(							\
		((struct parasite_dump_posix_timers_args*)args)->m, val);	\
	else									\
		ASSIGN_TYPED(							\
		((struct parasite_dump_posix_timers_args_compat*)args)->m, val);\
} while (0)

#define get_posix_timer_arg(out, m)						\
do {										\
	if (seized_native(ctl))							\
		ASSIGN_TYPED(							\
		out, ((struct parasite_dump_posix_timers_args*)args)->m);	\
	else									\
		ASSIGN_TYPED(							\
		out, ((struct parasite_dump_posix_timers_args_compat*)args)->m);\
} while (0)

static void encode_posix_timer(void *args, struct parasite_ctl *ctl,
		struct proc_posix_timer *vp, PosixTimerEntry *pte, int i)
{
	pte->it_id = vp->spt.it_id;
	pte->clock_id = vp->spt.clock_id;
	pte->si_signo = vp->spt.si_signo;
	pte->it_sigev_notify = vp->spt.it_sigev_notify;
	pte->sival_ptr = encode_pointer(vp->spt.sival_ptr);

	get_posix_timer_arg(pte->overrun, timer[i].overrun);

	get_posix_timer_arg(pte->isec, timer[i].val.it_interval.tv_sec);
	get_posix_timer_arg(pte->insec, timer[i].val.it_interval.tv_nsec);
	get_posix_timer_arg(pte->vsec, timer[i].val.it_value.tv_sec);
	get_posix_timer_arg(pte->vnsec, timer[i].val.it_value.tv_nsec);
}

int parasite_dump_posix_timers_seized(struct proc_posix_timers_stat *proc_args,
		struct parasite_ctl *ctl, struct pstree_item *item)
{
	CoreEntry *core = item->core[0];
	TaskTimersEntry *tte = core->tc->timers;
	PosixTimerEntry *pte;
	struct proc_posix_timer *temp;
	void *args = NULL;
	int args_size;
	int ret = 0;
	int i;

	if (core_alloc_posix_timers(tte, proc_args->timer_n, &pte))
		return -1;

	if (seized_native(ctl))
		args_size = posix_timers_dump_size(proc_args->timer_n);
	else
		args_size = posix_timers_compat_dump_size(proc_args->timer_n);
	args = compel_parasite_args_s(ctl, args_size);

	set_posix_timer_arg(args, ctl, timer_n, proc_args->timer_n);

	i = 0;
	list_for_each_entry(temp, &proc_args->timers, list) {
		set_posix_timer_arg(args, ctl, timer[i].it_id, temp->spt.it_id);
		i++;
	}

	ret = parasite_execute_daemon(PARASITE_CMD_DUMP_POSIX_TIMERS, ctl);
	if (ret < 0)
		goto end_posix;

	i = 0;
	list_for_each_entry(temp, &proc_args->timers, list) {
		posix_timer_entry__init(&pte[i]);
		encode_posix_timer(args, ctl, temp, &pte[i], i);
		tte->posix[i] = &pte[i];
		i++;
	}

end_posix:
	free_posix_timers(proc_args);
	return ret;
}

int parasite_dump_misc_seized(struct parasite_ctl *ctl, struct parasite_dump_misc *misc)
{
	struct parasite_dump_misc *ma;

	ma = compel_parasite_args(ctl, struct parasite_dump_misc);
	if (parasite_execute_daemon(PARASITE_CMD_DUMP_MISC, ctl) < 0)
		return -1;

	*misc = *ma;
	return 0;
}

struct parasite_tty_args *parasite_dump_tty(struct parasite_ctl *ctl, int fd, int type)
{
	struct parasite_tty_args *p;

	p = compel_parasite_args(ctl, struct parasite_tty_args);
	p->fd = fd;
	p->type = type;

	if (parasite_execute_daemon(PARASITE_CMD_DUMP_TTY, ctl) < 0)
		return NULL;

	return p;
}

int parasite_drain_fds_seized(struct parasite_ctl *ctl,
		struct parasite_drain_fd *dfds, int nr_fds, int off,
		int *lfds, struct fd_opts *opts)
{
	int ret = -1, size;
	struct parasite_drain_fd *args;

	size = drain_fds_size(dfds);
	args = compel_parasite_args_s(ctl, size);
	args->nr_fds = nr_fds;
	memcpy(&args->fds, dfds->fds + off, sizeof(int) * nr_fds);

	ret = __parasite_execute_daemon(PARASITE_CMD_DRAIN_FDS, ctl);
	if (ret) {
		pr_err("Parasite failed to drain descriptors\n");
		goto err;
	}

	ret = recv_fds(ctl->tsock, lfds, nr_fds, opts);
	if (ret)
		pr_err("Can't retrieve FDs from socket\n");

	ret |= __parasite_wait_daemon_ack(PARASITE_CMD_DRAIN_FDS, ctl);
err:
	return ret;
}

int parasite_get_proc_fd_seized(struct parasite_ctl *ctl)
{
	int ret = -1, fd;

	ret = __parasite_execute_daemon(PARASITE_CMD_GET_PROC_FD, ctl);
	if (ret) {
		pr_err("Parasite failed to get proc fd\n");
		return ret;
	}

	fd = recv_fd(ctl->tsock);
	if (fd < 0)
		pr_err("Can't retrieve FD from socket\n");
	if (__parasite_wait_daemon_ack(PARASITE_CMD_GET_PROC_FD, ctl)) {
		close_safe(&fd);
		return -1;
	}

	return fd;
}

/* This is officially the 50000'th line in the CRIU source code */

static bool task_is_trapped(int status, pid_t pid)
{
	if (WIFSTOPPED(status) && WSTOPSIG(status) == SIGTRAP)
		return true;

	pr_err("Task %d is in unexpected state: %x\n", pid, status);
	if (WIFEXITED(status))
		pr_err("Task exited with %d\n", WEXITSTATUS(status));
	if (WIFSIGNALED(status))
		pr_err("Task signaled with %d: %s\n",
			WTERMSIG(status), strsignal(WTERMSIG(status)));
	if (WIFSTOPPED(status))
		pr_err("Task stopped with %d: %s\n",
			WSTOPSIG(status), strsignal(WSTOPSIG(status)));
	if (WIFCONTINUED(status))
		pr_err("Task continued\n");

	return false;
}

static inline int is_required_syscall(user_regs_struct_t *regs, pid_t pid,
		const int sys_nr, const int sys_nr_compat)
{
	const char *mode = user_regs_native(regs) ? "native" : "compat";
	int req_sysnr = user_regs_native(regs) ? sys_nr : sys_nr_compat;

	pr_debug("%d (%s) is going to execute the syscall %lu, required is %d\n",
		pid, mode, REG_SYSCALL_NR(*regs), req_sysnr);

	return (REG_SYSCALL_NR(*regs) == req_sysnr);
}
/*
 * Trap tasks on the exit from the specified syscall
 *
 * tasks - number of processes, which should be trapped
 * sys_nr - the required syscall number
 * sys_nr_compat - the required compatible syscall number
 */
int parasite_stop_on_syscall(int tasks,
	const int sys_nr, const int sys_nr_compat,
	enum trace_flags trace)
{
	user_regs_struct_t regs;
	int status, ret;
	pid_t pid;

	if (tasks > 1)
		trace = TRACE_ALL;

	/* Stop all threads on the enter point in sys_rt_sigreturn */
	while (tasks) {
		pid = wait4(-1, &status, __WALL, NULL);
		if (pid == -1) {
			pr_perror("wait4 failed");
			return -1;
		}

		if (!task_is_trapped(status, pid))
			return -1;

		pr_debug("%d was trapped\n", pid);

		if (trace == TRACE_EXIT) {
			trace = TRACE_ENTER;
			pr_debug("`- Expecting exit\n");
			goto goon;
		}
		if (trace == TRACE_ENTER)
			trace = TRACE_EXIT;

		ret = ptrace_get_regs(pid, &regs);
		if (ret) {
			pr_perror("ptrace");
			return -1;
		}

		if (is_required_syscall(&regs, pid, sys_nr, sys_nr_compat)) {
			/*
			 * The process is going to execute the required syscall,
			 * the next stop will be on the exit from this syscall
			 */
			ret = ptrace(PTRACE_SYSCALL, pid, NULL, NULL);
			if (ret) {
				pr_perror("ptrace");
				return -1;
			}

			pid = wait4(pid, &status, __WALL, NULL);
			if (pid == -1) {
				pr_perror("wait4 failed");
				return -1;
			}

			if (!task_is_trapped(status, pid))
				return -1;

			pr_debug("%d was stopped\n", pid);
			tasks--;
			continue;
		}
goon:
		ret = ptrace(PTRACE_SYSCALL, pid, NULL, NULL);
		if (ret) {
			pr_perror("ptrace");
			return -1;
		}
	}

	return 0;
}

static int parasite_mmap_exchange(struct parasite_ctl *ctl, unsigned long size)
{
	int fd;

	ctl->remote_map = mmap_seized(ctl, NULL, size,
				      PROT_READ | PROT_WRITE | PROT_EXEC,
				      MAP_ANONYMOUS | MAP_SHARED, -1, 0);
	if (!ctl->remote_map) {
		pr_err("Can't allocate memory for parasite blob (pid: %d)\n", ctl->rpid);
		return -1;
	}

	ctl->map_length = round_up(size, page_size());

	fd = open_proc_rw(ctl->rpid, "map_files/%p-%p",
		 ctl->remote_map, ctl->remote_map + ctl->map_length);
	if (fd < 0)
		return -1;

	ctl->local_map = mmap(NULL, size, PROT_READ | PROT_WRITE,
			      MAP_SHARED | MAP_FILE, fd, 0);
	close(fd);

	if (ctl->local_map == MAP_FAILED) {
		ctl->local_map = NULL;
		pr_perror("Can't map remote parasite map");
		return -1;
	}

	return 0;
}

static int parasite_memfd_exchange(struct parasite_ctl *ctl, unsigned long size)
{
	void *where = (void *)ctl->ictx.syscall_ip + BUILTIN_SYSCALL_SIZE;
	u8 orig_code[MEMFD_FNAME_SZ] = MEMFD_FNAME;
	pid_t pid = ctl->rpid;
	unsigned long sret = -ENOSYS;
	int ret, fd, lfd;
	bool __maybe_unused compat_task = !seized_native(ctl);

	if (ctl->ictx.flags & INFECT_NO_MEMFD)
		return 1;

	BUILD_BUG_ON(sizeof(orig_code) < sizeof(long));

	if (ptrace_swap_area(pid, where, (void *)orig_code, sizeof(orig_code))) {
		pr_err("Can't inject memfd args (pid: %d)\n", pid);
		return -1;
	}

	ret = syscall_seized(ctl, __NR(memfd_create, compat_task), &sret,
			     (unsigned long)where, 0, 0, 0, 0, 0);

	if (ptrace_poke_area(pid, orig_code, where, sizeof(orig_code))) {
		fd = (int)(long)sret;
		if (fd >= 0)
			syscall_seized(ctl, __NR(close, compat_task), &sret,
					fd, 0, 0, 0, 0, 0);
		pr_err("Can't restore memfd args (pid: %d)\n", pid);
		return -1;
	}

	if (ret < 0)
		return ret;

	fd = (int)(long)sret;
	if (fd == -ENOSYS)
		return 1;
	if (fd < 0)
		return fd;

	ctl->map_length = round_up(size, page_size());
	lfd = open_proc_rw(ctl->rpid, "fd/%d", fd);
	if (lfd < 0)
		goto err_cure;

	if (ftruncate(lfd, ctl->map_length) < 0) {
		pr_perror("Fail to truncate memfd for parasite");
		goto err_cure;
	}

	ctl->remote_map = mmap_seized(ctl, NULL, size,
				      PROT_READ | PROT_WRITE | PROT_EXEC,
				      MAP_FILE | MAP_SHARED, fd, 0);
	if (!ctl->remote_map) {
		pr_err("Can't rmap memfd for parasite blob\n");
		goto err_curef;
	}

	ctl->local_map = mmap(NULL, size, PROT_READ | PROT_WRITE,
			      MAP_SHARED | MAP_FILE, lfd, 0);
	if (ctl->local_map == MAP_FAILED) {
		ctl->local_map = NULL;
		pr_perror("Can't lmap memfd for parasite blob");
		goto err_curef;
	}

	syscall_seized(ctl, __NR(close, compat_task), &sret, fd, 0, 0, 0, 0, 0);
	close(lfd);

	pr_info("Set up parasite blob using memfd\n");
	return 0;

err_curef:
	close(lfd);
err_cure:
	syscall_seized(ctl, __NR(close, compat_task), &sret, fd, 0, 0, 0, 0, 0);
	return -1;
}

int parasite_map_exchange(struct parasite_ctl *ctl, unsigned long size)
{
	int ret;

	ret = parasite_memfd_exchange(ctl, size);
	if (ret == 1) {
		pr_info("MemFD parasite doesn't work, goto legacy mmap\n");
		ret = parasite_mmap_exchange(ctl, size);
	}
	return ret;
}

int parasite_dump_cgroup(struct parasite_ctl *ctl, struct parasite_dump_cgroup_args *cgroup)
{
	int ret;
	struct parasite_dump_cgroup_args *ca;

	ca = compel_parasite_args(ctl, struct parasite_dump_cgroup_args);
	ret = parasite_execute_daemon(PARASITE_CMD_DUMP_CGROUP, ctl);
	if (ret) {
		pr_err("Parasite failed to dump /proc/self/cgroup\n");
		return ret;
	}

	*cgroup = *ca;
	return 0;
}

static unsigned long parasite_args_size = PARASITE_ARG_SIZE_MIN;
void parasite_ensure_args_size(unsigned long sz)
{
	if (parasite_args_size < sz)
		parasite_args_size = sz;
}

static int make_sigframe(void *arg, struct rt_sigframe *sf, struct rt_sigframe *rtsf, k_rtsigset_t *bs)
{
	return construct_sigframe(sf, rtsf, bs, (CoreEntry *)arg);
}

struct parasite_ctl *parasite_infect_seized(pid_t pid, struct pstree_item *item,
		struct vm_area_list *vma_area_list)
{
	struct parasite_ctl *ctl;
	unsigned long p;

	BUG_ON(item->threads[0].real != pid);

	p = get_exec_start(vma_area_list);
	if (!p) {
		pr_err("No suitable VM found\n");
		return NULL;
	}

	ctl = compel_prepare(pid);
	if (!ctl)
		return NULL;

	ctl->ictx.child_handler = sigchld_handler;
	ctl->ictx.p_sock = &dmpi(item)->netns->net.seqsk;
	ctl->ictx.save_regs = save_task_regs;
	ctl->ictx.make_sigframe = make_sigframe;
	ctl->ictx.regs_arg = item->core[0];
	ctl->ictx.syscall_ip = p;
	pr_debug("Parasite syscall_ip at %#lx\n", p);

	if (fault_injected(FI_NO_MEMFD))
		ctl->ictx.flags |= INFECT_NO_MEMFD;
	if (fault_injected(FI_PARASITE_CONNECT))
		ctl->ictx.flags |= INFECT_FAIL_CONNECT;

	parasite_ensure_args_size(dump_pages_args_size(vma_area_list));
	parasite_ensure_args_size(aio_rings_args_size(vma_area_list));

	if (compel_infect(ctl, item->nr_threads, parasite_args_size) < 0) {
		compel_cure(ctl);
		return NULL;
	}

	parasite_args_size = PARASITE_ARG_SIZE_MIN; /* reset for next task */
	memcpy(&item->core[0]->tc->blk_sigset, &ctl->orig.sigmask, sizeof(k_rtsigset_t));
	dmpi(item)->parasite_ctl = ctl;

	return ctl;
}

int ptrace_stop_pie(pid_t pid, void *addr, enum trace_flags *tf)
{
	int ret;

	if (fault_injected(FI_NO_BREAKPOINTS)) {
		pr_debug("Force no-breakpoints restore\n");
		ret = 0;
	} else
		ret = ptrace_set_breakpoint(pid, addr);
	if (ret < 0)
		return ret;

	if (ret > 0) {
		/*
		 * PIE will stop on a breakpoint, next
		 * stop after that will be syscall enter.
		 */
		*tf = TRACE_EXIT;
		return 0;
	}

	/*
	 * No breakpoints available -- start tracing it
	 * in a per-syscall manner.
	 */
	ret = ptrace(PTRACE_SYSCALL, pid, NULL, NULL);
	if (ret) {
		pr_perror("Unable to restart the %d process", pid);
		return -1;
	}

	*tf = TRACE_ENTER;
	return 0;
}
