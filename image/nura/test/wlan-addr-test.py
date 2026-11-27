#!/usr/bin/python3 -I
"""wlan-addr-test.py: data/wlan-addr/vibeos-wlan-addr on fixed command lines, with a stub ip first on PATH: the address
is deterministic, locally administered (first octet 02), differs from the Bluetooth one and is set on the interface
udev names; without a serial or an interface the script exits 0 and never calls ip. Reads and writes nothing outside a
temporary directory."""
import os, re, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.realpath(__file__))
S = os.path.join(HERE, '..', 'data', 'wlan-addr', 'vibeos-wlan-addr')
BT = os.path.join(HERE, '..', 'data', 'bt-addr', 'vibeos-bt-addr')
RULES = os.path.join(HERE, '..', 'data', 'wlan-addr', '90-vibeos-wlan-addr.rules')
fails = []


def run(cmdline, stub_dir, log, *args):
    cl = os.path.join(stub_dir, 'cmdline')
    open(cl, 'w').write(cmdline)
    if os.path.exists(log):
        os.unlink(log)
    env = {'PATH': stub_dir + ':/usr/bin:/bin', 'VIBEOS_CMDLINE': cl}
    return subprocess.run(['/bin/sh', S] + list(args), env=env, capture_output=True, text=True, timeout=30)


def check(name, ok):
    print('  %s: %s' % ('ok' if ok else 'FAIL', name))
    if not ok:
        fails.append(name)


def last(log):
    return open(log).read().splitlines()[-1].split() if os.path.exists(log) else []


with tempfile.TemporaryDirectory() as d:
    log = os.path.join(d, 'tool.log')
    for tool in ('ip', 'btmgmt'):
        stub = os.path.join(d, tool)
        open(stub, 'w').write('#!/bin/sh\necho %s "$@" >> %s\n[ "$1" = info ] || exit 0\n' % (tool, log))
        os.chmod(stub, 0o755)
    cl = 'console=tty0 androidboot.serialno=abc123 quiet'
    r = run(cl, d, log, 'wlan0')
    c = last(log)
    check('exit 0 and ip link set dev wlan0 address', r.returncode == 0 and c[:6] == ['ip', 'link', 'set', 'dev',
                                                                                       'wlan0', 'address'])
    addr = c[-1] if c else ''
    check('address is 02:xx:xx:xx:xx:xx', re.fullmatch(r'02(:[0-9a-f]{2}){5}', addr) is not None)
    run(cl, d, log, 'wlan0')
    check('same serial, same address', last(log)[-1:] == [addr])
    run('androidboot.serialno=other', d, log, 'wlan0')
    check('another serial, another address', last(log)[-1:] != [addr])
    open(os.path.join(d, 'cmdline'), 'w').write(cl)
    subprocess.run(['/bin/sh', BT], env={'PATH': d + ':/usr/bin:/bin', 'VIBEOS_CMDLINE': os.path.join(d, 'cmdline')},
                   capture_output=True, timeout=30)
    check('the Bluetooth address differs', last(log)[-1:] != [addr])
    r = run('console=tty0 quiet', d, log, 'wlan0')
    check('no serial: exit 0, ip never called', r.returncode == 0 and not os.path.exists(log))
    r = run(cl, d, log)
    check('no interface: exit 0, ip never called', r.returncode == 0 and not os.path.exists(log))
rule = open(RULES).read()
check('the rule matches wlan interfaces on add and passes the name',
      re.search(r'ACTION=="add".*SUBSYSTEM=="net".*KERNEL=="wlan\[0-9\]\*".*RUN\+="/usr/libexec/vibeos-wlan-addr \$name"',
                rule) is not None)
print('all ok' if not fails else '%d failed' % len(fails))
sys.exit(1 if fails else 0)
