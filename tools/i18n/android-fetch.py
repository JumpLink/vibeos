#!/usr/bin/python3 -I
# android-fetch.py CACHEDIR: the string resources of the Android apps VibeOS copies, in English and in the languages
# VibeOS is translated into, at the commits android-sources.json pins, for android-import.py.
#
# Each source goes into CACHEDIR/<name>/ under its paths in the repository; a file already there is kept. Only the
# resource XML files are fetched (from raw.githubusercontent.com, the trees from GitHub's API), and they are only ever
# parsed: nothing in CACHEDIR is run. CACHEDIR should be a directory of its own, outside the source tree.
import json
import os
import re
import sys
import time
import urllib.error
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
UA = {'User-Agent': 'vibeos-i18n-fetch'}
FILES = r'(strings[^/]*|cities|arrays|plurals)\.xml'


def get(url):
    for _attempt in range(4):
        try:
            with urllib.request.urlopen(urllib.request.Request(url, headers=UA), timeout=60) as r:
                return r.read()
        except urllib.error.HTTPError as e:
            if e.code == 404:
                return None
        except OSError:
            pass
        time.sleep(3)
    raise SystemExit(f'cannot fetch {url}')


def main():
    cache = sys.argv[1]
    manifest = json.load(open(os.path.join(HERE, 'android-sources.json')))
    langs = '|'.join(re.escape(lang) for lang in manifest['languages'])
    wanted = re.compile(r'(^|/)res(-[a-z]+)?/values(-(' + langs + r'))?/' + FILES + '$')
    for src in manifest['sources']:
        dest = os.path.join(cache, src['name'])
        if src.get('res'):
            # a repository too big to list: its resource directories are named
            paths = [f'{d}/values{suffix}/{name}' for d in src['res']
                     for suffix in [''] + ['-' + lang for lang in manifest['languages']]
                     for name in ('strings.xml', 'arrays.xml', 'plurals.xml')]
        else:
            data = get(f'https://api.github.com/repos/{src["repo"]}/git/trees/{src["commit"]}?recursive=1')
            tree = json.loads(data)
            if tree.get('truncated'):
                raise SystemExit(f'{src["name"]}: the tree listing is truncated')
            paths = [e['path'] for e in tree['tree'] if e.get('type') == 'blob' and wanted.search(e['path'])
                     and '/tests/' not in e['path'] and '/test/' not in e['path']]
        got = 0
        for path in paths:
            out = os.path.join(dest, path)
            if os.path.exists(out):
                got += 1
                continue
            data = get(f'https://raw.githubusercontent.com/{src["repo"]}/{src["commit"]}/{path}')
            if data is None:
                continue
            os.makedirs(os.path.dirname(out), exist_ok=True)
            with open(out, 'wb') as f:
                f.write(data)
            got += 1
        print(f'{src["name"]}: {got} files', flush=True)


main()
