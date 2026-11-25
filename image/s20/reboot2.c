/* reboot2 [recovery|download|bootloader]: restart with a reboot argument
 * (busybox reboot has none). syscon-reboot-mode turns it into S-Boot's
 * INFORM3 value. */
#include <unistd.h>
#include <sys/syscall.h>
#include <linux/reboot.h>
int main(int argc, char **argv)
{
	sync();
	if (argc > 1)
		return syscall(SYS_reboot, LINUX_REBOOT_MAGIC1, LINUX_REBOOT_MAGIC2,
			       LINUX_REBOOT_CMD_RESTART2, argv[1]);
	return syscall(SYS_reboot, LINUX_REBOOT_MAGIC1, LINUX_REBOOT_MAGIC2,
		       LINUX_REBOOT_CMD_RESTART, 0);
}
