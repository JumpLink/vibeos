#!/usr/bin/python3 -I
"""bt-addr-test.py: data/bt-addr/vibeos-bt-addr on fixed command lines, with a stub btmgmt first on PATH: the address
is deterministic, locally administered (first octet 02) and handed to btmgmt; without a serial the script exits 0
and never calls btmgmt. Reads and writes nothing outside a temporary directory."""
import os, re, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.realpath(__file__))
S = os.path.join(HERE, '..', 'data', 'bt-addr', 'vibeos-bt-addr')
fails = []


def run(cmdline, stub_dir, log):
    cl = os.path.join(stub_dir, 'cmdline')
    open(cl, 'w').write(cmdline)
    if os.path.exists(log):
        os.unlink(log)
    env = {'PATH': stub_dir + ':/usr/bin:/bin', 'VIBEOS_CMDLINE': cl}
    return subprocess.run(['/bin/sh', S], env=env, capture_output=True, text=True, timeout=30)


def check(name, ok):
    print('  %s: %s' % ('ok' if ok else 'FAIL', name))
    if not ok:
        fails.append(name)


with tempfile.TemporaryDirectory() as d:
    log = os.path.join(d, 'btmgmt.log')
    stub = os.path.join(d, 'btmgmt')
    open(stub, 'w').write('#!/bin/sh\necho "$@" >> %s\n[ "$1" = info ] || exit 0\n' % log)
    os.chmod(stub, 0o755)
    cl = 'console=tty0 androidboot.serialno=abc123 quiet'
    r = run(cl, d, log)
    calls = open(log).read().splitlines() if os.path.exists(log) else []
    sets = [c for c in calls if c.startswith('--index 0 public-addr ')]
    addr = sets[0].split()[-1] if sets else ''
    check('exit 0 and one public-addr call', r.returncode == 0 and len(sets) == 1)
    check('address is 02:xx:xx:xx:xx:xx', re.fullmatch(r'02(:[0-9a-f]{2}){5}', addr) is not None)
    run(cl, d, log)
    check('same serial, same address', open(log).read().splitlines()[-1].split()[-1] == addr)
    run('androidboot.serialno=other', d, log)
    check('another serial, another address', open(log).read().splitlines()[-1].split()[-1] != addr)
    r = run('console=tty0 quiet', d, log)
    check('no serial: exit 0, btmgmt never called', r.returncode == 0 and not os.path.exists(log))
print('all ok' if not fails else '%d failed' % len(fails))
sys.exit(1 if fails else 0)
