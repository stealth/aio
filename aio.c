/* IEEE 1003.1-2004 (aka POSIX realtime) aio_ implementation for Linux using
 * the io_ syscalls. This implementation is thread safe, although the standard
 * doesnt say anything about AIO and threads.
 *
 * (C) 2011 Sebastian Krahmer
 *
 * You may use this under the terms of the GPL.
 */
#define _GNU_SOURCE
#ifndef __USE_MISC
#define __USE_MISC
#endif
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/times.h>
#include <sys/select.h>
#include <sys/syscall.h>

#include "aio.h"

#ifndef ANDROID
#include <sys/eventfd.h>

extern int sigqueue(pid_t, int, const union sigval);

#else

#ifndef __NR_eventfd2
#define __NR_eventfd2 (__NR_SYSCALL_BASE + 356)
#endif

inline int eventfd(unsigned int initval, int flags)
{
	return syscall(__NR_eventfd2, initval, flags);
}

int sigqueue(pid_t pid, int sig, const union sigval value)
{
	siginfo_t si = {
		.si_signo = sig,
		.si_code = SI_QUEUE,
		.si_pid = getpid(),
		.si_uid = getuid(),
		.si_value = value
	};
	return syscall(__NR_rt_sigqueueinfo, pid, sig, &si);
}

#endif

extern int fsync(int);
extern int kill(pid_t, int);
extern int pselect(int, fd_set *, fd_set *, fd_set *, const struct timespec *, const sigset_t *);


static const int TID_MAX = 33000;
static char __child_stack[4096];

/* We want a reader/writer lock. Of the uin32_t integer lock value
 * the lower 16 bits count the number of writers holding a lock and
 * the upper 16 bits count the number of readers. Only one writer is allowed
 * if there are no readers, but multiple readers are allowed if there is
 * no writer.
 */
enum {
	CTX_UNLOCKED		= 0,
	CTX_LOCKED_W		= 1,
	CTX_LOCKED_R		= (1<<16),
	CTX_WLOCKED_MASK	= CTX_LOCKED_R - 1,

	AIO_UNINITIALIZED	= 0,
	AIO_INITIALIZING	= 1,
	AIO_INITIALIZED		= 2
};

/* The node of a context list per thread */
struct __ctx {
	aio_context_t ctx_id;
	int aio_fildes, efd;
	pid_t tid;
	struct sigevent aio_sigevent;
	long int aio_return;
	int aio_error;
	struct iocb iocb;
	struct __ctx *next;
};


/* atomics, concurrently accessed */
static int __init_lock = AIO_UNINITIALIZED;
static struct __ctx **__ctxs = NULL;
static uint32_t *__ctx_locks = NULL;

/* non-atomics, only accessed reading not not at all */
static int __watcher_tid = 0;
static pid_t __likely_tid = 0;


static struct __ctx *get_ctx_list_lock_w(pid_t tid)
{
	/* writers are exclusive i.e. there must be no lock at all */
	while (__sync_fetch_and_add(__ctx_locks + tid, CTX_LOCKED_W) != CTX_UNLOCKED)
		__sync_fetch_and_sub(__ctx_locks + tid, CTX_LOCKED_W);
	return __ctxs[tid];
}


static struct __ctx *get_ctx_list_lock_r(pid_t tid)
{
	/* any writer lock (i.e. any of the lower 16 bits set)? */
	while (__sync_fetch_and_add(__ctx_locks + tid, CTX_LOCKED_R) & CTX_WLOCKED_MASK)
		__sync_fetch_and_sub(__ctx_locks + tid, CTX_LOCKED_R);
	return __ctxs[tid];
}


static void put_ctx_list_lock_r(pid_t tid)
{
	__sync_fetch_and_sub(__ctx_locks + tid, CTX_LOCKED_R);
}


static void put_ctx_list_lock_w(pid_t tid)
{
	__sync_fetch_and_sub(__ctx_locks + tid, CTX_LOCKED_W);
}


static int notify_finished(struct __ctx *c)
{
	int64_t one = 1;

	/* If a event fd is registered in the ctx struct, someone is on
	 * aio_suspend(), so notify this sleeping thread via event fd.
	 * Errors dont matter. They may  happen as aio_suspend() only holds a
	 * reader lock which means that we could write to a c->efd of -1.
	 * However this is not a fault.
	 */
	int fd = __sync_fetch_and_add(&c->efd, 0);
	if (fd > 0)
		write(fd, &one, sizeof(one));

	/* SIGEV_NONE as per standard */
	if (c->aio_sigevent.sigev_signo != 0 && c->aio_sigevent.sigev_notify != SIGEV_NONE)
		sigqueue(c->tid, c->aio_sigevent.sigev_signo, c->aio_sigevent.sigev_value);
	return 0;
}


static int __watcher_event_fd = -1;

static int __aio_watcher(void *vp)
{
	struct io_event event;
	struct timespec to;
	pid_t i = 0;
	int64_t i64 = 0;
	int r = 0, done = 0;
	struct __ctx *c = NULL;

	for (;;) {
reloop:
		/* Since we flagged IOCB_FLAG_RESFD, we will receive event on
		 * eventfd if kernel finds something ready. We also check
		 * for < 0 since inside the loop we might fetch an event that
		 * is not signaled when inside the loop and therefore have
		 * more results than expected.
		 */
		if (i64 <= 0) {
			if (read(__watcher_event_fd, &i64, sizeof(i64)) < 0)
				continue;
		}

		/* Optimization: in order to not walk thru all TID lists in the common case,
		 * we start with our parent thread which most likely started the I/O operation.
		 * We could also start from TID 1, but this will mostly look up empty lists and wastes
		 * cycles.
		 */
		done = 0;
		for (i = __likely_tid; i != __likely_tid || !done; i = (i+1)%TID_MAX) {
			done = 1;
			for (c = get_ctx_list_lock_r(i); c != NULL; c = c->next) {
				to.tv_sec = 0;
				to.tv_nsec = 1;

				/* If its not in EINPROGRESS, its already finished */
				if (__sync_fetch_and_add(&c->aio_error, 0) != EINPROGRESS)
					continue;
				r = syscall(__NR_io_getevents, c->ctx_id, 1, 1, &event, &to);

				/* Since we only have a readlock for c, the following assignments need
				 * to be atomic and in that order!
				 */
				if (r > 0) {
					/* atomic 'c->aio_return = event.res;'
					 * (must have been inited with -1)
					 */
					__sync_val_compare_and_swap(&c->aio_return, -1, event.res);
					if (event.res > 0) {
						/* c->aio_error = 0; */
						__sync_val_compare_and_swap(&c->aio_error, EINPROGRESS, 0);
					} else {
						/* c->aio_error = -(int)event.res; */
						__sync_val_compare_and_swap(&c->aio_error, EINPROGRESS, -(int)event.res);
					}
					notify_finished(c);
				}
				if (r > 0)
					--i64;
				if (i64 <= 0) {
					put_ctx_list_lock_r(i);
					goto reloop;
				}
			}
			put_ctx_list_lock_r(i);
		}
	}

	return 0;
}


static void __aio_atexit(void)
{
	kill(__watcher_tid, 9);
}

static void __aio_init()
{
	if (__sync_val_compare_and_swap(&__init_lock, AIO_UNINITIALIZED, AIO_INITIALIZING) != AIO_UNINITIALIZED)
		return;

	/* atomics, but protected by above lock */
	__ctxs = calloc(TID_MAX + 1, sizeof(struct __ctx *));
	__ctx_locks = calloc(TID_MAX + 1, sizeof(uint32_t));

	__watcher_event_fd = eventfd(0, 0);
	__likely_tid = syscall(__NR_gettid);
	__watcher_tid = clone(__aio_watcher, __child_stack + sizeof(__child_stack), CLONE_VM|CLONE_FILES, NULL);
	if (__watcher_tid > 0)
		atexit(__aio_atexit);

	__sync_val_compare_and_swap(&__init_lock, AIO_INITIALIZING, AIO_INITIALIZED);
}


static int __aio_read_write(struct aiocb *aiocbp, int opcode)
{
	int r = 0;
	struct iocb _iocb[1], *iocbp = _iocb;
	struct __ctx *c = NULL;
	pid_t tid = 0;

	while (__sync_fetch_and_add(&__init_lock, 0) != AIO_INITIALIZED)
		__aio_init();

	errno = 0;
	if (!aiocbp) {
		errno = EINVAL;
		return -1;
	}
	tid = syscall(__NR_gettid);

	memset(&aiocbp->ctx_id, 0, sizeof(aiocbp->ctx_id));
	if ((r = syscall(__NR_io_setup, 1, &aiocbp->ctx_id)) < 0) {
		errno = -r;
		return -1;
	}

	memset(iocbp, 0, sizeof(*iocbp));
	iocbp->aio_buf = (size_t)aiocbp->aio_buf;
	iocbp->aio_nbytes = aiocbp->aio_nbytes;
	iocbp->aio_offset = aiocbp->aio_offset;
	iocbp->aio_fildes = aiocbp->aio_fildes;
	iocbp->aio_lio_opcode = opcode;
	iocbp->aio_reqprio = aiocbp->aio_reqprio;

	/* We want notifications by kernel to avoid busy waiting */
	iocbp->aio_resfd = __watcher_event_fd;
	iocbp->aio_flags |= IOCB_FLAG_RESFD;

	aiocbp->tid = tid;

	if ((r = syscall(__NR_io_submit, aiocbp->ctx_id, 1, &iocbp)) < 0) {
		errno = -r;
		return -1;
	}

	c = (struct __ctx *)calloc(1, sizeof(struct __ctx));
	memcpy(&c->iocb, iocbp, sizeof(*iocbp));

	c->aio_error = aiocbp->aio_error = EINPROGRESS;
	c->aio_return = aiocbp->aio_return = -1;

	c->aio_fildes = aiocbp->aio_fildes;
	c->ctx_id = aiocbp->ctx_id;
	c->aio_sigevent = aiocbp->aio_sigevent;
	c->tid = tid;
	c->efd = -1;		/* no event fd yet */
	__sync_synchronize();

	c->next = get_ctx_list_lock_w(tid);
	__ctxs[tid] = c;
	put_ctx_list_lock_w(tid);
	return 0;
}


int aio_read(struct aiocb *aiocbp)
{
	return __aio_read_write(aiocbp, IOCB_CMD_PREAD);
}


int aio_write(struct aiocb *aiocbp)
{
	return __aio_read_write(aiocbp, IOCB_CMD_PWRITE);
}


int aio_fsync(int op, struct aiocb *aiocbp)
{
	int r = 0;

	errno = 0;
	if (!aiocbp) {
		errno = EINVAL;
		return -1;
	}
	switch (op) {
	case O_SYNC:
		r = fsync(aiocbp->aio_fildes);
		break;
#ifdef O_DSYNC
#if O_DSYNC != O_SYNC
	case O_DSYNC:
		r = fdatasync(aiocbp->aio_fildes);
		break;
#endif
#endif
	default:
		errno = EINVAL;

	}
	return r;
}


int aio_error(struct aiocb *aiocbp)
{
	int r = -1;
	struct __ctx *c = NULL;

	while (__sync_fetch_and_add(&__init_lock, 0) != AIO_INITIALIZED)
		__aio_init();

	errno = EINVAL;
	if (!aiocbp) {
		return -1;
	}

	/* If there is an error triggered by lio_listio(), it cannot be
	 * placed inside our context list already, so we wont find it there.
	 * Therefore lio_listio() errors are returned directly.
	 */
	if (aiocbp->lio_error)
		return aiocbp->lio_error;

	c = get_ctx_list_lock_r(aiocbp->tid);
	for (; c != NULL;) {
		if (c->ctx_id == aiocbp->ctx_id) {
			errno = 0;
			r = aiocbp->aio_error = __sync_fetch_and_add(&c->aio_error, 0);
			break;
		}
		c = c->next;
	}
	put_ctx_list_lock_r(aiocbp->tid);
	return r;
}


int aio_cancel(int fd, struct aiocb *aiocbp)
{
	struct io_event result;
	struct __ctx *c = NULL, **old_c = NULL, *c2 = NULL;
	int r = AIO_CANCELED, sr = 0;
	pid_t tid = 0;

	while (__sync_fetch_and_add(&__init_lock, 0) != AIO_INITIALIZED)
		__aio_init();

	errno = 0;

	/* special case: cancel all operations for this fd (in this thread) */
	if (!aiocbp) {
		tid = syscall(__NR_gettid);
		c = get_ctx_list_lock_w(tid);
		if (!c) {
			put_ctx_list_lock_w(tid);
			errno = EBADF;
			return -1;
		}
		old_c = &__ctxs[tid];
		r = AIO_ALLDONE;
		for (; c != NULL;) {
			if (c->aio_fildes == fd) {
				if ((sr = syscall(__NR_io_cancel, c->ctx_id, &c->iocb, &result) < 0)) {
					r = AIO_NOTCANCELED;
					old_c = &c->next;
					c = c->next;
				} else {
					syscall(__NR_io_destroy, c->ctx_id);
					c2 = c;
					*old_c = c->next;
					c = c->next;
					free(c2);
				}
				__sync_synchronize();
			}
		}
	} else {
		tid = aiocbp->tid;
		c = get_ctx_list_lock_w(tid);
		old_c = &__ctxs[tid];
		for (; c != NULL; c = c->next) {
			if (c->ctx_id == aiocbp->ctx_id) {
				if ((sr = syscall(__NR_io_cancel, c->ctx_id, &c->iocb, &result) < 0)) {
					r = AIO_NOTCANCELED;
				} else {
					syscall(__NR_io_destroy, c->ctx_id);
					*old_c = c->next;
					free(c);
				}
				__sync_synchronize();
				break;
			}
			old_c = &c->next;
		}
	}
	put_ctx_list_lock_w(tid);
	return r;
}


static int do_aio_suspend(const struct aiocb *const cblist[], int n, const struct timespec *timeout)
{
	int i = 0, hits = 0, r = 0, evfd = -1, ready = 0;
	int64_t i64 = 0;
	const struct aiocb *aiocbp = NULL;
	struct __ctx *c = NULL;
	fd_set rset;

	while (__sync_fetch_and_add(&__init_lock, 0) != AIO_INITIALIZED)
		__aio_init();

	errno = 0;

	/* For each of the aiocb's, set the event fd where the watcher thread
	 * will write us if something gets ready.
	 */
	for (i = 0; i < n && !ready; ++i) {
		aiocbp = cblist[i];
		if (!aiocbp)
			continue;
		/* We need a writer lock here. Not because of the
		 * 'c->efd = evfd' which we could make atomic, but b/c there
		 * is a race between the 'c->aio_error == EINPROGRESS' case
		 * and the 'c->efd = evfd' where 'c' could become ready and
		 * the notification can get lost from the watcher thread
		 * and this thread is waiting in the upcoming pselect() then
		 * forever.
		 * Having a writer lock on this list will prevent the watcher
		 * from changing c's state.
		 */
		for (c = get_ctx_list_lock_w(aiocbp->tid); c != NULL; c = c->next) {
			if (c->ctx_id == aiocbp->ctx_id) {
				/* If already finished, nothing to do */
				if (__sync_fetch_and_add(&c->aio_error, 0) != EINPROGRESS) {
					ready = 1;
					break;
				}
				/* We shift opening of eventfd until here to have
				 * a fast path for the c->aio_error == EINPROGRESS case
				 * above which saves us two syscalls.
				 */
				if (evfd < 0) {
					if ((evfd = eventfd(0, 0)) < 0) {
						put_ctx_list_lock_w(aiocbp->tid);
						return -1;
					}
				}
				/* atomic c->efd = evfd; */
				__sync_lock_test_and_set(&c->efd, evfd);
				++hits;
				break;
			}
		}
		put_ctx_list_lock_w(aiocbp->tid);
	}

	if (!hits && !ready) {
		errno = EAGAIN;
		return -1;
	}

	if (!ready) {
		/* pselect because it has "struct timeval *timeout" as well */
		FD_ZERO(&rset);
		FD_SET(evfd, &rset);
		r = pselect(evfd + 1, &rset, NULL, NULL, timeout, NULL);
	}

	/* reset event fd for each aiocb */
	for (i = 0; i < n && evfd > 0; ++i) {
		aiocbp = cblist[i];
		if (!aiocbp)
			continue;
		/* Use reader-lock now but set efd atomic (see above comment).*/
		for (c = get_ctx_list_lock_r(aiocbp->tid); c != NULL; c = c->next) {
			if (c->ctx_id == aiocbp->ctx_id) {
				__sync_lock_test_and_set(&c->efd, -1);
				break;
			}
		}
		put_ctx_list_lock_r(aiocbp->tid);
	}

	if (ready) {
		if (evfd >= 0)
			close(evfd);
		return 0;
	}

	/* The pselect() return. Timeout or error? */
	if (r == 0) {
		errno = EAGAIN;
		return -1;
	} else if (r < 0) {
		errno = EINTR;
		return -1;
	}

	read(evfd, &i64, sizeof(i64));
	close(evfd);
	return 0;
}


int aio_suspend(const struct aiocb *const cblist[], int n, const struct timespec *timeout)
{
	return do_aio_suspend(cblist, n, timeout);
}


/* aio_return() may be only called once for a given aiocb */
long int aio_return(struct aiocb *aiocbp)
{
	struct __ctx *c = NULL, **old_c = NULL;
	long int r = 0;

	while (__sync_fetch_and_add(&__init_lock, 0) != AIO_INITIALIZED)
		__aio_init();

	errno = EINVAL;
	if (!aiocbp) {
		return -1;
	}

	/* We are going to modify the list, so we need a writer lock. */
	c = get_ctx_list_lock_w(aiocbp->tid);
	old_c = &__ctxs[aiocbp->tid];
	for (; c != NULL; c = c->next) {
		if (c->ctx_id == aiocbp->ctx_id) {
			errno = 0;
			*old_c = c->next;
			r = __sync_fetch_and_add(&c->aio_return, 0);
			syscall(__NR_io_destroy, c->ctx_id);
			free(c);
			__sync_synchronize();
			break;
		}
		old_c = &c->next;
	}
	put_ctx_list_lock_w(aiocbp->tid);
	return r;
}


/* without -std=c99, GCC has the "restrict" keyoword not available */
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 199901L
int lio_listio(int mode, struct aiocb *restrict const list[restrict], int nent, struct sigevent *sig)
#else
int lio_listio(int mode, struct aiocb *const list[], int nent, struct sigevent *sig)
#endif
{
	int i = 0, r = 0, aio_listio_max = -1, aio_max = -1;
	errno = 0;

	/* if glibc doesnt properly define them */
	if ((aio_listio_max = sysconf(_SC_AIO_LISTIO_MAX)) < 0)
		aio_listio_max = 1024*1024;
	if ((aio_max = sysconf(_SC_AIO_MAX)) < 0)
		aio_max = 10*1024*1024;

	if (nent <= 0 || nent > aio_listio_max ||
	    (mode != LIO_WAIT && mode != LIO_NOWAIT)) {
		errno = EINVAL;
		return -1;
	}

	if (nent > aio_max) {
		errno = EAGAIN;
		return -1;
	}

	/* Set lio_error rather than aio_error! */
	for (i = 0; i < nent; ++i) {
		if (sig)
			list[i]->aio_sigevent = *sig;
		if (list[i]->aio_lio_opcode == LIO_READ) {
			if ((r = __aio_read_write(list[i], IOCB_CMD_PREAD)) < 0) {
				list[i]->lio_error = r;
				errno = EAGAIN;
				return -1;
			}
		} else if (list[i]->aio_lio_opcode == LIO_WRITE) {
			if ((r = __aio_read_write(list[i], IOCB_CMD_PWRITE)) < 0) {
				list[i]->lio_error = r;
				errno = EAGAIN;
				return -1;
			}
		} else if (list[i]->aio_lio_opcode != LIO_NOP) {
			list[i]->lio_error = EIO;
			errno = EIO;
			return -1;
		}
	}
	if (mode == LIO_NOWAIT)
		return 0;

	r = 0;
	for (i = 0; i < nent; ++i) {
		if (list[i]) {
			r -= do_aio_suspend((const struct aiocb *const *)&list[i], 1, NULL);
		}
	}
	if (r < 0)
		return -1;
	return 0;
}

