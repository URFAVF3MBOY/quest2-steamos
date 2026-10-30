/*
 * qlog - persist the kernel log (/dev/kmsg: kernel + systemd + anything that logs to kmsg)
 *        into a raw region of a block device, so it can be read on a PC after any boot.
 *
 *   qlog DEVICE OFFSET_BYTES SIZE_BYTES
 *
 * Region layout (OFFSET must be 4096-aligned):
 *   block 0        header: "Q2LOG1\n" + a short note (rewritten once at start)
 *   block 1..N     plain text, one "[sec.usec] message" per line, zero padded
 * The block after the current one is always zeroed, so the reader stops at the first all-zero block.
 * Runs forever (survives switch_root: it only keeps fds open), exits when the region is full or the
 * device disappears (e.g. USB switched back to device mode).
 *
 * Test hook: QL_SRC=/path/file reads "one kmsg record per line" from a file instead of /dev/kmsg.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define BLK 4096

static int dev;
static off_t base;
static uint64_t nblocks;          /* data blocks available (excluding header) */
static uint64_t cur;              /* current data block index */
static unsigned char blk[BLK];
static size_t used;
static int dirty;
static unsigned char zero[BLK];

static void die_dev(const char *what)
{
	fprintf(stderr, "qlog: %s: %s (stopping)\n", what, strerror(errno));
	exit(1);
}

static void wr(uint64_t idx, const void *p)       /* idx 0 = header */
{
	ssize_t r = pwrite(dev, p, BLK, base + (off_t)idx * BLK);
	if (r != BLK) die_dev("write");
}

static void flush_cur(void)
{
	if (!dirty) return;
	unsigned char t[BLK];
	memset(t, 0, BLK);
	memcpy(t, blk, used);
	wr(1 + cur, t);
	dirty = 0;
}

static void put(const char *s, size_t n)
{
	while (n) {
		size_t k = BLK - used;
		if (k > n) k = n;
		memcpy(blk + used, s, k);
		used += k; s += k; n -= k; dirty = 1;
		if (used == BLK) {
			flush_cur();
			cur++;
			if (cur >= nblocks) { fprintf(stderr, "qlog: region full\n"); exit(0); }
			used = 0; memset(blk, 0, BLK);
			if (cur + 1 < nblocks) wr(2 + cur, zero);   /* terminator: the block AFTER the current one reads as empty */
		}
	}
}

/* "pri,seq,usec,flags;message\n KEY=val..." -> "[sssss.uuuuuu] message\n" */
static void record(const char *r, ssize_t len)
{
	const char *semi = memchr(r, ';', len);
	if (!semi) return;
	unsigned long long usec = 0;
	int commas = 0;
	for (const char *p = r; p < semi; p++) {
		if (*p == ',') { commas++; continue; }
		if (commas == 2 && *p >= '0' && *p <= '9') usec = usec * 10 + (*p - '0');
	}
	const char *msg = semi + 1;
	const char *end = r + len;
	const char *nl = memchr(msg, '\n', end - msg);       /* drop dictionary lines after first \n */
	size_t mlen = (nl ? nl : end) - msg;
	char head[40];
	int hl = snprintf(head, sizeof head, "[%5llu.%06llu] ", usec / 1000000ULL, usec % 1000000ULL);
	put(head, hl);
	put(msg, mlen);
	put("\n", 1);
}

static double now(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

int main(int argc, char **argv)
{
	if (argc != 4) { fprintf(stderr, "usage: qlog DEVICE OFFSET SIZE\n"); return 2; }
	base = (off_t)strtoull(argv[2], NULL, 0);
	uint64_t size = strtoull(argv[3], NULL, 0);
	if (base % BLK || size < 4 * BLK) { fprintf(stderr, "qlog: bad offset/size\n"); return 2; }
	nblocks = size / BLK - 1;

	dev = open(argv[1], O_RDWR | O_SYNC | O_CLOEXEC);
	if (dev < 0) die_dev("open device");

	const char *src = getenv("QL_SRC");
	int kfd = open(src ? src : "/dev/kmsg", O_RDONLY | O_CLOEXEC | (src ? 0 : O_NONBLOCK));
	if (kfd < 0) die_dev("open kmsg");

	unsigned char hdr[BLK];
	memset(hdr, 0, BLK);
	snprintf((char *)hdr, BLK, "Q2LOG1\nquest2 kernel/systemd log, newest boot only. Text starts at the next 4096-byte block.\n");
	wr(0, zero);
	wr(1, zero);                                  /* current block */
	if (nblocks > 1) wr(2, zero);                 /* terminator after it */
	wr(0, hdr);
	fprintf(stderr, "qlog: logging %s -> %s @%llu size %llu\n", src ? src : "/dev/kmsg", argv[1],
		(unsigned long long)base, (unsigned long long)size);

	if (src) {                                   /* test mode: one record per line */
		FILE *f = fdopen(kfd, "r");
		char *line = NULL; size_t cap = 0; ssize_t n;
		while ((n = getline(&line, &cap, f)) > 0) record(line, n);
		flush_cur();
		return 0;
	}

	static char buf[16384];
	double last = now();
	for (;;) {
		struct pollfd p = { .fd = kfd, .events = POLLIN };
		int pr = poll(&p, 1, 500);
		if (pr > 0) {
			for (;;) {
				ssize_t n = read(kfd, buf, sizeof buf - 1);
				if (n < 0) {
					if (errno == EPIPE) continue;     /* records were overwritten, carry on */
					break;                            /* EAGAIN: drained */
				}
				if (n > 0) record(buf, n);
			}
		}
		if (dirty && (pr == 0 || now() - last > 1.0)) { flush_cur(); last = now(); }
	}
}
