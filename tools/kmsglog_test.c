/* Android native regression tests; all I/O stays in a temporary directory. */
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static char dir[256], input[300], output[300], mutex_path[300];
static int test_poll(struct pollfd *, nfds_t, int);
static ssize_t test_read(int, void *, size_t);
static ssize_t test_write(int, const void *, size_t);
static int test_clock(clockid_t, struct timespec *);
static int test_sleep(const struct timespec *, struct timespec *);
#define KMSG input
#define OUT_DIR dir
#define OUT output
#define MUTEX mutex_path
#define LIFETIME_S 10
#define SIZE_CAP 8192
#define main follower_main
#define poll test_poll
#define read test_read
#define write test_write
#define clock_gettime test_clock
#define nanosleep test_sleep
#include "../app/src/main/jni/kmsglog.c"
#undef main
#undef poll
#undef read
#undef write
#undef clock_gettime
#undef nanosleep

enum { IDLE, POLL_DENIED, READ_DENIED, INTERRUPTED, DATA, PARTIAL,
       OVERRUN_ONCE, OVERRUN_ALWAYS, HANGUP, INVALID, WRITE_FAILED };
static int mode, polls, reads, sleeps, writes;
static long clock_ms;
static int test_clock(clockid_t id, struct timespec *ts)
{ (void)id; ts->tv_sec=clock_ms/1000; ts->tv_nsec=(clock_ms%1000)*1000000; return 0; }
static int test_sleep(const struct timespec *ts, struct timespec *left)
{ (void)left; sleeps++; clock_ms+=ts->tv_nsec/1000000; return 0; }
static int test_poll(struct pollfd *p, nfds_t n, int timeout)
{
    assert(n==1 && timeout>0 && timeout<=10000);
    /* The singleton lock must cover the entire follower session. */
    int lock=open(mutex_path,O_RDWR); assert(lock>=0);
    assert(flock(lock,LOCK_EX|LOCK_NB)<0 && errno==EWOULDBLOCK); close(lock);
    polls++;
    if(mode==POLL_DENIED){errno=EACCES;return -1;}
    if(mode==INTERRUPTED && polls==1){errno=EINTR;return -1;}
    if(mode==HANGUP || mode==INVALID){p->revents=mode==HANGUP?POLLHUP:POLLNVAL;return 1;}
    if(mode==READ_DENIED || mode==WRITE_FAILED || mode==OVERRUN_ALWAYS
            || ((mode==DATA || mode==PARTIAL) && polls==1)
            || (mode==OVERRUN_ONCE && polls<=2)) {
        p->revents=(mode==OVERRUN_ONCE || mode==OVERRUN_ALWAYS)?POLLERR:POLLIN;return 1;
    }
    /* An idle follower waits once for its remaining lifetime, not every 50ms. */
    assert(timeout>=9800 || mode==OVERRUN_ONCE);
    clock_ms+=timeout;return 0;
}
static ssize_t test_read(int fd, void *buf, size_t size)
{
    (void)fd;reads++;assert(size==2048);
    if(mode==READ_DENIED){errno=EACCES;return -1;}
    if(mode==OVERRUN_ALWAYS || (mode==OVERRUN_ONCE && reads==1)){errno=EPIPE;return -1;}
    if(reads==1 || (mode==OVERRUN_ONCE && reads==2)){memset(buf,'A',size);return (ssize_t)size;}
    errno=EAGAIN;return -1;
}
static ssize_t test_write(int fd, const void *buf, size_t size)
{
    if(size>1){
        writes++;
        if(mode==WRITE_FAILED){errno=ENOSPC;return -1;}
        if(mode==PARTIAL){if(writes==1){errno=EINTR;return -1;}if(size>17)size=17;}
    }
    return write(fd,buf,size);
}
static void run_case(int chosen, int expected)
{
    mode=chosen;polls=reads=sleeps=writes=0;clock_ms=0;
    int fd=open(input,O_CREAT|O_TRUNC|O_WRONLY,0600);assert(fd>=0);close(fd);
    fd=open(output,O_CREAT|O_TRUNC|O_WRONLY,0600);assert(fd>=0);close(fd);
    assert(follower_main()==expected);
    fd=open(mutex_path,O_RDWR);assert(fd>=0);assert(flock(fd,LOCK_EX|LOCK_NB)==0);close(fd);
    if(chosen==POLL_DENIED)assert(polls==1 && reads==0);
    if(chosen==READ_DENIED)assert(polls==1 && reads==1);
    if(chosen==IDLE)assert(polls==1 && reads==0);
    if(chosen==INTERRUPTED)assert(polls==2 && reads==0);
    if(chosen==HANGUP || chosen==INVALID)assert(reads==0 && polls==1);
    if(chosen==OVERRUN_ALWAYS)assert(reads==17 && sleeps==16);
    if(chosen==DATA || chosen==PARTIAL || chosen==OVERRUN_ONCE){
        FILE *f=fopen(output,"rb");assert(f);char b[2049];assert(fread(b,1,sizeof(b),f)==sizeof(b));
        for(int i=0;i<2048;i++)assert(b[i]=='A');assert(b[2048]=='\n');fclose(f);
        if(chosen==OVERRUN_ONCE)assert(sleeps==1);
    }
    printf("PASS case %d\n",chosen);
}
int main(void)
{
    strcpy(dir,"/data/local/tmp/df-kmsg-tests-XXXXXX");assert(mkdtemp(dir));
    snprintf(input,sizeof(input),"%s/input",dir);snprintf(output,sizeof(output),"%s/log",dir);
    snprintf(mutex_path,sizeof(mutex_path),"%s/lock",dir);
    run_case(POLL_DENIED,1);run_case(READ_DENIED,1);run_case(IDLE,0);
    run_case(INTERRUPTED,0);run_case(DATA,0);run_case(PARTIAL,0);
    run_case(OVERRUN_ONCE,0);run_case(OVERRUN_ALWAYS,1);
    run_case(HANGUP,1);run_case(INVALID,1);run_case(WRITE_FAILED,1);
    int fd=open(mutex_path,O_RDWR);assert(fd>=0);assert(flock(fd,LOCK_EX|LOCK_NB)==0);
    polls=0;assert(follower_main()==0 && polls==0);close(fd);puts("PASS singleton exclusion");
    unlink(input);mode=IDLE;assert(follower_main()==1);
    run_case(IDLE,0);puts("PASS open-failure cleanup and stale-file reuse");
    unlink(input);unlink(output);unlink(mutex_path);rmdir(dir);
    puts("PASS all kmsglog regression tests");return 0;
}
