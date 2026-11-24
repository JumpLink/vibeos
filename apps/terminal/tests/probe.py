#!/usr/bin/env python3
# probe.py MODE LOG: the program touch.js runs in the Terminal. It asks for MODE's private modes, as less (alt),
# less with application cursor keys (altapp) or htop (mouse, mouse-legacy) would, clears the screen and parks the
# cursor at row 10, column 5 (so the cursor's rectangle shows the shell where the grid is), writes "READY rows
# columns" to LOG, then appends each read of what the terminal sends it, in hex, one read a line, until a read has a
# q in it.
import os
import sys
import termios
import tty

MODES = {
    'alt': '\033[?1049h',
    'altapp': '\033[?1049h\033[?1h',
    'mouse': '\033[?1049h\033[?1000;1006h',
    'mouse-legacy': '\033[?1049h\033[?1000h',
}
UNDO = '\033[?1000;1006l\033[?1l\033[?1049l'

mode, log = sys.argv[1], sys.argv[2]
fd = sys.stdin.fileno()
saved = termios.tcgetattr(fd)
tty.setraw(fd)
try:
    os.write(1, (MODES[mode] + '\033[2J\033[10;5H').encode())
    size = os.get_terminal_size(fd)
    with open(log, 'w') as out:
        out.write(f'READY {size.lines} {size.columns}\n')
        out.flush()
        while True:
            data = os.read(fd, 4096)
            if not data:
                break
            out.write(data.hex() + '\n')
            out.flush()
            if b'q' in data:
                break
finally:
    os.write(1, UNDO.encode())
    termios.tcsetattr(fd, termios.TCSADRAIN, saved)
