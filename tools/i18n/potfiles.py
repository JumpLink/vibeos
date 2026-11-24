#!/usr/bin/python3 -I
# potfiles.py SRCDIR: prints a po/POTFILES.in for the component in SRCDIR: its data/*.desktop.in files and the source
# files that call gettext. Files kept outside SRCDIR (another component's sources built in) are added by hand.
import glob
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from component import Component  # noqa: E402

c = Component.__new__(Component)
c.srcdir = os.path.abspath(sys.argv[1])
print('# the files with translatable strings')
for p in sorted(glob.glob(os.path.join(c.srcdir, 'data', '*.desktop.in'))):
    print(os.path.relpath(p, c.srcdir))
for p in c.sources_with_gettext():
    print(p)
