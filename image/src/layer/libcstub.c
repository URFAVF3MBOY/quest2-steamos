#define S(n) void n(void){}
S(malloc) S(calloc) S(free) S(memcpy) S(memset) S(strcmp) S(strlen) S(getenv) S(vsnprintf) S(snprintf) S(write) S(open) S(close)
S(mmap) S(munmap) S(getpid) S(unlink) S(clock_gettime) S(nanosleep) S(pthread_create) S(pthread_mutex_init) S(pthread_mutex_lock)
S(pthread_mutex_unlock) S(pthread_cond_init) S(pthread_cond_wait) S(pthread_cond_timedwait) S(pthread_cond_broadcast)
#define S(n) void n(void){}
S(memfd_create) S(ftruncate) S(dup) S(readlink) S(strstr)
S(syscall)
S(read)
S(socket) S(connect) S(recvmsg) S(send)
S(opendir) S(readdir) S(closedir)
