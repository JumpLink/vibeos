#!/usr/bin/python3 -I
"""cmdline-test.py: clean_cmdline of nura-to-vibeos on command lines as Nura's boot images carry them (the OnePlus
6T's, and one without any splash setting): pmOS's own parameters and Nura's splash settings dropped, the kernel's
own kept in order, Plymouth off exactly once. Imports the script; reads and writes nothing."""
import importlib.machinery, importlib.util, os, sys

p = os.path.join(os.path.dirname(os.path.realpath(__file__)), '..', 'nura-to-vibeos')
spec = importlib.util.spec_from_loader('nura_to_vibeos', importlib.machinery.SourceFileLoader('nura_to_vibeos', p))
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)
fails = []


def check(c, want, want_dropped):
    got, dropped = m.clean_cmdline(c)
    good = got == want and dropped == want_dropped
    print('  %s: %r\n      -> %r, dropped %r' % ('ok' if good else 'FAIL', c, got, dropped))
    if not good:
        fails.append(c)


check('pmos_boot_uuid=x pmos_root_uuid=y pmos_rootfsopts=defaults splash plymouth.ignore-serial-consoles '
      'plymouth.prefer-fbcon console=ttyMSM0,115200',
      'console=ttyMSM0,115200 plymouth.enable=0',
      ['pmos_boot_uuid=x', 'pmos_root_uuid=y', 'pmos_rootfsopts=defaults', 'splash', 'plymouth.ignore-serial-consoles',
       'plymouth.prefer-fbcon'])
check('console=tty0 root=/dev/sda2 rw quiet msm_drm.dsi_display0=panel',
      'console=tty0 quiet msm_drm.dsi_display0=panel plymouth.enable=0', ['root=/dev/sda2', 'rw'])
check('', 'plymouth.enable=0', [])
print('all ok' if not fails else '%d failed' % len(fails))
sys.exit(1 if fails else 0)
