"""init-case.py ROOT CMDLINE WAIT OUT: one case of init-test.sh. Runs ROOT/test-init (which sources ROOT/stubs.sh,
then the initramfs's own /init; aarch64, under qemu-user via binfmt) with CMDLINE as its /proc/cmdline, inside the
user, mount and network namespaces that unshare -rmn gives (no privileges on the host, nothing reaches the host's
network, mounts private), and writes what happened to OUT (JSON): the init's kernel-log lines, whether it ended
(it ends when it execs the root's systemd, a stub here) or still ran after WAIT seconds, the stubs' log.
The case directory carries:
  - stubs for what would touch hardware or leave the chroot (mount: an ext4 mount binds ROOT/fakedisk, everything
    else is logged and succeeds; umount, e2fsck, resize2fs, telnetd, dropbear, udhcpd, ...), each logging its
    command line to ROOT/stublog
  - a fake sysfs: sys/class/block/<part>/uevent (PARTNAME=...), sys/class/udc/<udc>, sys/fs/pstore
  - fakedisk/: the root partition (lib/systemd/systemd, and what a case leaves on it)
/dev/kmsg is a FIFO read here, /dev/tty0 a plain file. After WAIT seconds every process left inside ROOT is killed.
Must run as root in a new user namespace."""
import json, os, signal, subprocess, sys, threading, time

R, CMDLINE, WAIT, OUT = sys.argv[1], sys.argv[2], float(sys.argv[3]), sys.argv[4]
R = os.path.realpath(R)
res = {'cmdline': CMDLINE, 'kmsg': [], 'tty0': ''}


def sh(*a):
    return subprocess.run(a, capture_output=True, text=True)


assert os.getuid() == 0
sh('ip', 'link', 'add', 'usb0', 'type', 'veth', 'peer', 'name', 'usb1')
sh('ip', 'link', 'set', 'lo', 'up')
open(R + '/proc/cmdline', 'w').write(CMDLINE + '\n')
for n in ('null', 'zero', 'urandom', 'random'):
    open(R + '/dev/' + n, 'w').close()
    assert sh('mount', '--bind', '/dev/' + n, R + '/dev/' + n).returncode == 0
os.mkfifo(R + '/dev/kmsg')
open(R + '/dev/tty0', 'w').close()


def reader():
    while True:
        with open(R + '/dev/kmsg', 'rb') as f:
            for line in f:
                res['kmsg'].append('%5.1f %s' % (time.time() - t0, line.decode(errors='replace').rstrip()))


t0 = time.time()
threading.Thread(target=reader, daemon=True).start()
log = open(OUT + '.init-output.txt', 'wb')
init = subprocess.Popen(['/usr/sbin/chroot', R, '/test-init'], stdin=subprocess.DEVNULL, stdout=log, stderr=log,
                        start_new_session=True, env={'HOME': '/', 'TERM': 'linux'})
try:
    res['init_exit'] = init.wait(timeout=WAIT)
    res['init_ended_after_s'] = round(time.time() - t0, 1)
except subprocess.TimeoutExpired:
    res['init_exit'] = None


def inside():
    ps = []
    for p in os.listdir('/proc'):
        if not p.isdigit():
            continue
        try:
            if os.readlink('/proc/%s/root' % p) != R:
                continue
            a = open('/proc/%s/cmdline' % p, 'rb').read().split(b'\0')
        except OSError:
            continue
        a = [x.decode(errors='replace') for x in a if x]
        if a and 'binfmt' in a[0]:
            a = a[1:]
        ps.append((int(p), ' '.join(a)[:100]))
    return sorted(ps)


time.sleep(1)
res['running_at_end'] = [c for _, c in inside()]
for _ in range(3):
    for pid, _c in inside():
        try:
            os.kill(pid, signal.SIGKILL)
        except OSError:
            pass
    time.sleep(0.5)
res['left_running'] = [c for _, c in inside()]
try:
    res['stublog'] = open(R + '/stublog').read().splitlines()
except OSError:
    res['stublog'] = []
res['tty0'] = open(R + '/dev/tty0', errors='replace').read()
m = R + '/fakedisk/var/lib/s10e/boot-attempt'
res['marker_after'] = open(m).read().strip() if os.path.exists(m) else None
json.dump(res, open(OUT, 'w'), indent=1)
os._exit(0)
