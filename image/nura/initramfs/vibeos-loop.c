/* vibeos-loop FILE OFFSET SIZELIMIT [ro]: binds FILE (a partition or an image file) from byte OFFSET, SIZELIMIT
 * bytes long (0: to its end), to a free loop device and prints its path. Busybox's losetup has no size limit, and
 * the root inside a partition's own GPT (the POCO's "linux") must end where its GPT entry ends: the first boot
 * grows the file system to the device's size. Build: aarch64-linux-gnu-gcc -static -Os -s -o vibeos-loop vibeos-loop.c */
#include <fcntl.h>
#include <linux/loop.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	if (argc < 4) { fprintf(stderr, "usage: vibeos-loop FILE OFFSET SIZELIMIT [ro]\n"); return 2; }
	int ro = argc > 4 && !strcmp(argv[4], "ro");
	int f = open(argv[1], ro ? O_RDONLY : O_RDWR);
	if (f < 0) { perror(argv[1]); return 1; }
	int c = open("/dev/loop-control", O_RDWR);
	if (c < 0) { perror("/dev/loop-control"); return 1; }
	for (int tries = 0; tries < 8; tries++) {
		int n = ioctl(c, LOOP_CTL_GET_FREE);
		if (n < 0) { perror("LOOP_CTL_GET_FREE"); return 1; }
		char dev[32];
		snprintf(dev, sizeof dev, "/dev/loop%d", n);
		int l = open(dev, O_RDWR);
		if (l < 0) {	/* devtmpfs makes the node; without it, make it */
			mknod(dev, S_IFBLK | 0600, makedev(7, n));
			l = open(dev, O_RDWR);
		}
		if (l < 0) { perror(dev); return 1; }
		struct loop_config cfg;
		memset(&cfg, 0, sizeof cfg);
		cfg.fd = f;
		cfg.info.lo_offset = strtoull(argv[2], 0, 0);
		cfg.info.lo_sizelimit = strtoull(argv[3], 0, 0);
		cfg.info.lo_flags = ro ? LO_FLAGS_READ_ONLY : 0;
		if (ioctl(l, LOOP_CONFIGURE, &cfg) == 0) { puts(dev); return 0; }
		/* kernels before 5.8: LOOP_SET_FD + LOOP_SET_STATUS64 */
		if (ioctl(l, LOOP_SET_FD, f) == 0) {
			if (ioctl(l, LOOP_SET_STATUS64, &cfg.info) == 0) { puts(dev); return 0; }
			perror("LOOP_SET_STATUS64");
			ioctl(l, LOOP_CLR_FD, 0);
			return 1;
		}
		close(l);	/* taken meanwhile: the next free one */
	}
	fprintf(stderr, "no free loop device\n");
	return 1;
}
