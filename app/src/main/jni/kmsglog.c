/* Persistent kernel-log follower.
 *
 * Spawned by the dfroot UMH shell at ko-init time (root, SELinux
 * permissive). Reads /dev/kmsg message-by-message and appends each one to
 * the public Download dir with a per-message fsync: whatever the kernel
 * prints reaches the disk within milliseconds and survives even a watchdog
 * hard reset, which takes the whole page cache (and pstore) with it.
 *
 * pstore is structurally absent on this device, so this daemon is the only
 * witness of the lead-up to a wedge/panic. The final panic() print itself
 * lands after userspace is frozen and is intentionally out of scope - the
 * last fsynced line marks the entry point of the hang.
 *
 * /dev/dfkmsg is a flock-protected inode on tmpfs. The kernel releases
 * the lock on exit, including SIGKILL; stale files cannot block a later run.
 *
 * Growth limits: after root succeeds this daemon would otherwise follow
 * dmesg forever and the log file would grow unbounded. Two caps, whichever
 * hits first ends the session: a file-size cap (existing content larger
 * than the cap is rotated away at startup) and a wall-clock lifetime. */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifndef KMSG
#define KMSG        "/dev/kmsg"
#endif
#ifndef MUTEX
#define MUTEX       "/dev/dfkmsg"
#endif
#ifndef OUT_DIR
#define OUT_DIR     "/data/media/0/Download/DirtyFrag"
#endif
#ifndef OUT
#define OUT         OUT_DIR "/dmesg_live.txt"
#endif
#ifndef SIZE_CAP
#define SIZE_CAP    (4 * 1024 * 1024)   /* 4 MiB: several full boot logs */
#endif
#ifndef LIFETIME_S
#define LIFETIME_S  1800                /* 30 min per boot is plenty */
#endif

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int write_all(int fd, const char *buf, size_t size)
{
    while (size) {
        ssize_t n = write(fd, buf, size);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        buf += n;
        size -= (size_t)n;
    }
    return 0;
}

static void stop_reason(int fd, const char *operation, int error)
{
    char buf[192];
    int n = snprintf(buf, sizeof(buf), "kmsglog: %s%s%s, stopping\n",
                     operation, error ? ": " : "", error ? strerror(error) : "");
    if (n > 0 && n < (int)sizeof(buf)) {
        (void)write_all(fd, buf, (size_t)n);
        (void)fsync(fd);
    }
}

int main(void)
{
    int result = 0, k = -1, o = -1;
    int mfd = open(MUTEX, O_CREAT | O_RDWR | O_CLOEXEC, 0644);
    if (mfd < 0) return 1;
    if (flock(mfd, LOCK_EX | LOCK_NB) < 0) {
        int saved = errno;
        close(mfd);
        return saved == EWOULDBLOCK ? 0 : 1;
    }
    /* Keep this inode: unlinking a locked file races with new followers. */
    (void)ftruncate(mfd, 0);
    (void)write_all(mfd, "1", 1);

    mkdir(OUT_DIR, 0755);           /* UMH shell normally did this already */

    k = open(KMSG, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (k < 0) { result = 1; goto cleanup; }
    /* Start at the END of the ring: only what happens from now on. */
    lseek(k, 0, SEEK_END);

    /* Size accounting starts from the file as it exists: if an old log is
     * already over the cap, rotate it away and start fresh. */
    struct stat st;
    long size = (stat(OUT, &st) == 0) ? (long)st.st_size : 0;
    if (size > SIZE_CAP) {
        char old[512];
        snprintf(old, sizeof(old), "%s.1", OUT);
        rename(OUT, old);           /* best effort; ignore failures */
        size = 0;
    }

    o = open(OUT, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0666);
    if (o < 0) { result = 1; goto cleanup; }

    long t0 = now_ms();
    char buf[2049]; /* one extra byte for a missing newline */
    struct pollfd p = { .fd = k, .events = POLLIN };
    unsigned overruns = 0;
    for (;;) {
        long remaining = (long)LIFETIME_S * 1000 - (now_ms() - t0);
        if (remaining <= 0) {
            stop_reason(o, "lifetime cap reached", 0);
            break;
        }
        /* Wait for data or the deadline, with no periodic idle polling. */
        int r = poll(&p, 1, (int)remaining);
        if (r < 0) {
            if (errno == EINTR) continue;
            stop_reason(o, "poll failed", errno);
            result = 1;
            break;
        }
        if (r == 0) continue;
        if (p.revents & (POLLNVAL | POLLHUP)) {
            stop_reason(o, "kernel log descriptor closed", 0);
            result = 1;
            break;
        }
        /* /dev/kmsg may report POLLERR on ring overrun. A read advances its
         * cursor and returns EPIPE; a permission failure must instead exit. */
        if (!(p.revents & (POLLIN | POLLERR))) {
            stop_reason(o, "unexpected poll events", 0);
            result = 1;
            break;
        }
        for (;;) {
            ssize_t n = read(k, buf, sizeof(buf) - 1);
            if (n > 0) {
                overruns = 0;
                if (buf[n - 1] != '\n') { buf[n] = '\n'; n++; }
                if (write_all(o, buf, (size_t)n) < 0 || fsync(o) < 0) {
                    result = 1;
                    goto cleanup;
                }
                size += n;
                if (size >= SIZE_CAP) {
                    stop_reason(o, "size cap reached", 0);
                    goto cleanup;
                }
                continue;
            }
            if (n < 0 && errno == EINTR) continue;
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
            if (n < 0 && errno == EPIPE && ++overruns <= 16) {
                /* Bound repeated overruns even when a driver stays ready. */
                struct timespec delay = { .tv_nsec = 100000000 };
                (void)nanosleep(&delay, NULL);
                break;
            }
            stop_reason(o, n == 0 ? "kernel log ended" : "read failed", n < 0 ? errno : 0);
            result = 1;
            goto cleanup;
        }
    }
cleanup:
    if (o >= 0) close(o);
    if (k >= 0) close(k);
    close(mfd);
    return result;
}
