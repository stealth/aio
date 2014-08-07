AIO
===

This is an implementation for Linux of the IEEE 1003.1-2004 (aka POSIX realtime) AIO
functions, utilizing 2.6 kernel's `io_` syscalls rather than using __pthreads__ as in _glibc_.
However, we still need a watcher thread since the Linux `io_` syscalls do not match
the AIO specification at all. Therefore we need an extra thread to signal the I/O thread
about events. Nevertheless this scales much better than _glibc_ creating a new thread
for each asynchronous submit.

Build
-----

Just `make` and you will find `aio.o` which you can link to your programs using `aio_` calls.
You also need to include the proper _aio.h_ file of course. Do not link against `-lrt` as
this contains glibc's version of AIO.
This aio lib is thread safe. It also builds and works for __Android__.
AIO only builds with the `gcc` (still, it is __C99__ code :) since I use some of the GCC
intrinsics for atomic read/write operations (remember: thread safe!).


Misc
----

I wrote this code for my own purposes (basically a fast backup/restore and forensic tool)
so its (C) Sebastian Krahmer. However you can use this library under the terms of
the GPL.

Note that you need to open files with the `O_DIRECT` flag for _aio_ to work
(the test files skip that but you need it to ensure real async operation in kernel)
as well as the memory buffers need to be aligned for disk blocks.


