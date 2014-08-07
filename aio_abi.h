/* Based on kernel's aio_abi.h, and defining the same __LINUX__AIO_ABI_H
 * to be compatible with double-includes. However this is not a copy&paste
 * but the struct names and members must match nevertheless.
 */
#ifndef __LINUX__AIO_ABI_H
#define __LINUX__AIO_ABI_H

#include <stdint.h>
#include <asm/byteorder.h>
typedef unsigned long aio_context_t;

enum {
	IOCB_CMD_PREAD = 0,
	IOCB_CMD_PWRITE = 1,
	IOCB_CMD_FSYNC = 2,
	IOCB_CMD_FDSYNC = 3,
	IOCB_CMD_NOOP = 6,
	IOCB_CMD_PREADV = 7,
	IOCB_CMD_PWRITEV = 8,
};


#define IOCB_FLAG_RESFD 1

struct io_event {
	uint64_t data;
	uint64_t obj;
	int64_t res;
	int64_t res2;
};

#ifdef __LITTLE_ENDIAN
#define PAD(x,y) x, y
#elif defined(__BIG_ENDIAN)
#define PAD(x,y) y, x
#endif

struct iocb {
	uint64_t aio_data;
	uint32_t PAD(aio_key, aio_reserved1);
	uint16_t aio_lio_opcode;
	int16_t aio_reqprio;
	uint32_t aio_fildes;
	uint64_t aio_buf;
	uint64_t aio_nbytes;
	int64_t aio_offset;

	uint64_t aio_reserved2;
	uint32_t aio_flags;
	uint32_t aio_resfd;
};

#endif

