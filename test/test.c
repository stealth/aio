/* test module for aio implementation for aio_error(), aio_return() and aio_suspend() */
#include "aio.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <signal.h>


void die(const char *s)
{
	perror(s);
	exit(errno);
}


void sig_int(int x)
{
	return;
}


int main()
{
	int fd, i = 0, e = 0;
	long int r = 0;
	struct stat st;
	char *buf = NULL;
	struct aiocb *a = NULL;
	struct sigaction sa;

#ifdef ANDROID
	if ((fd = open("/data/system/packages.xml", O_RDONLY)) < 0)
#else
	if ((fd = open("/etc/passwd", O_RDONLY)) < 0)
#endif
		die("open");
	fstat(fd, &st);

	a = calloc(1, sizeof(*a)*st.st_size);
	buf = calloc(1, st.st_size);

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = sig_int;
	sa.sa_flags = SA_RESTART;
	sigaction(SIGINT, &sa, NULL);

	for (i = 0; i < st.st_size; ++i) {
		a[i].aio_fildes = fd;
		a[i].aio_buf = &buf[i];
		a[i].aio_nbytes = 1;
		a[i].aio_offset = i;
		a[i].aio_sigevent.sigev_signo = SIGINT;
		if (aio_read(&a[i]) < 0)
			die("aio_read");
		if (i % 3 == 0)
			printf("cancel: %d\n", aio_cancel(fd, &a[i]));
	}

	printf("Requests submitted. Now fetching status...\n");
	for (i = 0; i < st.st_size; ++i) {
#ifdef AIO_SUSPEND
		struct aiocb *cal = &a[i];
		aio_suspend((const struct aiocb * const*)&cal, 1, NULL);
#else
		do {
			e = aio_error(&a[i]);
		} while (e == EINPROGRESS);
		if (e == EINVAL) {
			continue;
		}
#endif
		e = aio_error(&a[i]);
		r = aio_return(&a[i]);
	}

	printf("%s", buf);
	free(buf);
	free(a);
	return 0;
}

