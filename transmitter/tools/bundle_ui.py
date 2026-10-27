#!/usr/bin/env python3
"""Bundle the web UI into one gzipped HTML file (embedded in the firmware).

    bundle_ui.py <web dir> <out .html.gz>

Inlines every <link rel="stylesheet" href="X"> and <script src="X"></script>
of index.html, then gzips with a fixed mtime so the output (and the version
hash the firmware derives from it) only changes when the sources do.
"""
import gzip
import os
import re
import sys


def main():
    web, out = sys.argv[1], sys.argv[2]

    def read(name):
        with open(os.path.join(web, name), encoding='utf-8') as f:
            return f.read().replace('\r\n', '\n')

    html = read('index.html')
    html = re.sub(r'<link rel="stylesheet" href="([^"]+)">',
                  lambda m: '<style>\n' + read(m.group(1)) + '</style>', html)
    html = re.sub(r'<script src="([^"]+)"></script>',
                  lambda m: '<script>\n' + read(m.group(1)).replace('</script', '<\\/script') + '</script>',
                  html)
    if re.search(r'<(link|script)[^>]+(href|src)=', html):
        sys.exit('bundle_ui: unresolved external reference left in index.html')

    raw = html.encode('utf-8')
    gz = gzip.compress(raw, compresslevel=9, mtime=0)
    tmp = out + '.tmp'
    with open(tmp, 'wb') as f:
        f.write(gz)
    os.replace(tmp, out)
    print('bundle_ui: %d bytes -> %d bytes gzipped' % (len(raw), len(gz)))


if __name__ == '__main__':
    main()
