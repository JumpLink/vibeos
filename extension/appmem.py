#!/usr/bin/python3
# appmem.py KEY...: the memory each app in recents uses, printed as JSON {KEY: bytes}.
#
# A KEY is a window's process id, or "waydroid.PACKAGE" for an Android app. Recents runs this in a process of its own
# under a timeout: reading /proc of a process stuck in the kernel blocks whoever reads it, and that must never be the
# shell (its gestures would stop). A helper that hangs only leaves the figures out.
#
# An app's memory is the proportional set size (PSS) of its process and every process it started (a browser's web
# processes): pages shared between processes are split among them, so the apps' figures add up.
import json
import os
import re
import sys


def read(path):
    try:
        with open(path, 'rb') as f:
            return f.read()
    except OSError:
        return None


def children_by_parent():
    """Every process's children, from the parent pid in /proc/PID/stat."""
    kids = {}
    for pid in os.listdir('/proc'):
        if not pid.isdigit():
            continue
        stat = read(f'/proc/{pid}/stat')
        if not stat:
            continue
        # the command name is in parentheses and may hold spaces: the fields after it start at the last ')'
        ppid = stat[stat.rfind(b')') + 2:].split()[1].decode()
        kids.setdefault(ppid, []).append(pid)
    return kids


def tree_pss(pid, kids):
    total, todo = 0, [pid]
    while todo:
        p = todo.pop()
        m = re.search(rb'^Pss:\s+(\d+) kB', read(f'/proc/{p}/smaps_rollup') or b'', re.M)
        if m:
            total += int(m.group(1)) * 1024
        todo.extend(kids.get(p, []))
    return total


def android_app(package):
    """An Android app runs in Waydroid's container under Android's own uids: the process that opened the window is
    only waydroid's bridge, the same for every app. Count the package's processes ("pkg" and "pkg:service"); their
    smaps are not ours to read, so their private memory from status (RssAnon), which anyone may read."""
    package = package.encode()
    total = 0
    for p in os.listdir('/proc'):
        if not p.isdigit():
            continue
        name = (read(f'/proc/{p}/cmdline') or b'').split(b'\0')[0]
        if name == package or name.startswith(package + b':'):
            m = re.search(rb'^RssAnon:\s+(\d+) kB', read(f'/proc/{p}/status') or b'', re.M)
            if m:
                total += int(m.group(1)) * 1024
    return total


def main(keys):
    kids = children_by_parent()
    result = {}
    for key in keys:
        if key.startswith('waydroid.'):
            total = android_app(key[len('waydroid.'):])
        elif key.isdigit():
            total = tree_pss(key, kids)
        else:
            continue
        if total:
            result[key] = total
    print(json.dumps(result))


main(sys.argv[1:])
