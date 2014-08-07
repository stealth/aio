/* IEEE 1003.1-2004 (aka POSIX realtime) aio_ implementation for Linux using
 * the io_ syscalls. This implementation is thread safe, although the standard
 * doesnt say anything about AIO and threads.
 *
 * (C) 2011 Sebastian Krahmer
 *
 * You may use this under the terms of the GPL.
 */
#ifndef __aio_h__
#define __aio_h__

#include <sys/types.h>
#include <stddef.h>
#include <stdint.h>

#ifdef ANDROID
typedef uint64_t __u64;
typedef int64_t __s64;
#endif

#if 0
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 199901L
#include <asm/siginfo.h>
#endif
#endif


#include <signal.h>
#include <time.h>

#ifdef ANDROID
#include "aio_abi.h"
#else
#include <linux/aio_abi.h>
#endif


#ifndef ANDROID
#ifndef __timespec_defined
#define __timespec_defined 1

struct timespec {
	time_t tv_sec;
	long int tv_nsec;
};

#endif
#endif

struct aiocb
{
	int aio_fildes;
	int aio_lio_opcode;
	int aio_reqprio;
	void *aio_buf;
	size_t aio_nbytes;
	struct sigevent aio_sigevent;
	size_t aio_offset;
	int aio_error, lio_error;
	long int aio_return;

	aio_context_t ctx_id;
	pid_t tid;
};


enum {
	AIO_CANCELED,
	AIO_NOTCANCELED,
	AIO_ALLDONE
};


enum {
	LIO_READ,
	LIO_WRITE,
	LIO_NOP
};

enum {
	LIO_WAIT,
	LIO_NOWAIT
};


int aio_read(struct aiocb *aiocbp);

int aio_write(struct aiocb *aiocbp);

int aio_fsync(int op, struct aiocb *aiocbp);

int aio_error(struct aiocb *aiocbp);

int aio_cancel(int fd, struct aiocb *aiocbp);

int aio_suspend(const struct aiocb *const cblist[], int n, const struct timespec *timeout);

long int aio_return(struct aiocb *aiocbp);

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 199901L
int lio_listio(int mode, struct aiocb *restrict const list[restrict], int nent, struct sigevent * sig);
#else
int lio_listio(int mode, struct aiocb *const list[], int nent, struct sigevent * sig);
#endif

#endif


