#!/usr/bin/python3 -I
"""device-mask-test.py: the mask column of devices.txt in nura-to-vibeos: read per phone (the OnePlus 6T masks
81voltd, the others nothing), and written into a payload root as empty unit files, a second time without change.
Imports the script; writes only into a temporary directory."""
import importlib.machinery, importlib.util, os, sys, tempfile

p = os.path.join(os.path.dirname(os.path.realpath(__file__)), '..', 'nura-to-vibeos')
spec = importlib.util.spec_from_loader('nura_to_vibeos', importlib.machinery.SourceFileLoader('nura_to_vibeos', p))
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)
fails = []


def check(name, got, want):
    good = got == want
    print('  %s: %s: %r' % ('ok' if good else 'FAIL', name, got))
    if not good:
        fails.append(name)


check('fajita: masks 81voltd', m.device_row('oneplus-fajita')['mask'], ['81voltd.service'])
check('fajita: dtbo still read', m.device_row('oneplus-fajita')['dtbo'], 'clear')
check('enchilada: masks nothing', m.device_row('oneplus-enchilada')['mask'], [])

with tempfile.TemporaryDirectory() as P:
    f = os.path.join(P, 'etc/systemd/system/81voltd.service')
    check('first run: masked', m.mask_units(P, ['81voltd.service']), ['81voltd.service'])
    check('an empty file', (os.path.isfile(f), os.path.getsize(f)), (True, 0))
    check('second run: already there', m.mask_units(P, ['81voltd.service']), [])
    check('no units: nothing', m.mask_units(P, []), [])

try:
    m.mask_units(tempfile.gettempdir(), ['../x.service'])
    check('a path is refused', 'accepted', 'refused')
except SystemExit:
    check('a path is refused', 'refused', 'refused')

print('all ok' if not fails else '%d failed' % len(fails))
sys.exit(1 if fails else 0)
