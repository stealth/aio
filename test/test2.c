/* test module for aio implementation for lio_listio() */
#include "../aio.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>


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
	int fd, i = 0;
	struct stat st;
	char *buf = NULL;
	struct aiocb **a = NULL;
	struct sigaction sa;

#ifdef ANDROID
	if ((fd = open("/etc/permissions/platform.xml", O_RDONLY)) < 0)
#else
	if ((fd = open("/etc/passwd", O_RDONLY)) < 0)
#endif
		die("open");
	fstat(fd, &st);

	a = (struct aiocb **)calloc(1, sizeof(struct aiocb *)*st.st_size);
	buf = calloc(1, st.st_size);

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = sig_int;
	sa.sa_flags = SA_RESTART;
	sigaction(SIGINT, &sa, NULL);

	for (i = 0; i < st.st_size; ++i) {
		a[i] = calloc(1, sizeof(struct aiocb));
		a[i]->aio_fildes = fd;
		a[i]->aio_buf = &buf[i];
		a[i]->aio_nbytes = 1;
		a[i]->aio_offset = i;
		a[i]->aio_sigevent.sigev_signo = SIGINT;
		a[i]->aio_lio_opcode = LIO_READ;
	}

	if (lio_listio(LIO_WAIT, a, (int)st.st_size, NULL) < 0) {
		perror("lio_listio");
		exit(errno);
	}
	printf("%s", buf);
	free(buf);
	free(a);
	return 0;
}


