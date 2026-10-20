/*
 * qrw - clear the read-only flag of a btrfs subvolume (what `btrfs property set -ts PATH ro false` does).
 *
 * Valve's rootfs is a READ-ONLY btrfs subvolume. Mounting it with -o rw succeeds, but every write then fails with
 * EROFS until the subvolume flag is cleared. Needs no btrfs-progs; one ioctl.
 *
 *   qrw PATH        PATH = mount point of the subvolume (or any path inside it)
 * prints the old flags; exit 0 = flag is now clear.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#ifndef BTRFS_IOC_SUBVOL_GETFLAGS
#define BTRFS_IOCTL_MAGIC 0x94
#define BTRFS_IOC_SUBVOL_GETFLAGS _IOR(BTRFS_IOCTL_MAGIC, 25, uint64_t)
#define BTRFS_IOC_SUBVOL_SETFLAGS _IOW(BTRFS_IOCTL_MAGIC, 26, uint64_t)
#endif
#define SUBVOL_RDONLY (1ULL << 1)

int main(int argc, char **argv)
{
	if (argc != 2) { fprintf(stderr, "usage: qrw PATH\n"); return 2; }
	int fd = open(argv[1], O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (fd < 0) { fprintf(stderr, "qrw: open %s: %s\n", argv[1], strerror(errno)); return 1; }
	uint64_t fl = 0;
	if (ioctl(fd, BTRFS_IOC_SUBVOL_GETFLAGS, &fl) < 0) {
		fprintf(stderr, "qrw: GETFLAGS: %s\n", strerror(errno));
		return 1;
	}
	printf("subvol flags 0x%llx (%s)", (unsigned long long)fl, (fl & SUBVOL_RDONLY) ? "READ-ONLY" : "writable");
	if (fl & SUBVOL_RDONLY) {
		fl &= ~SUBVOL_RDONLY;
		if (ioctl(fd, BTRFS_IOC_SUBVOL_SETFLAGS, &fl) < 0) {
			printf(" -> SETFLAGS failed: %s\n", strerror(errno));
			return 1;
		}
		printf(" -> cleared");
	}
	printf("\n");
	return 0;
}
