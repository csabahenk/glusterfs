import os
import os.path
import sys

fl = os.listdir(os.path.dirname(sys.argv[0]) or '.')
fl.sort()
for f in fl:
    if f[-3:] != '.py' or f[0] == '_':
        continue
    m = f[:-3]
    sys.stdout.write('importing %s ...' %  m)
    __import__(m)
    print(' OK.')
