import os
print('MSYSTEM in env:', 'MSYSTEM' in os.environ)
if 'MSYSTEM' in os.environ:
    print('MSYSTEM=', os.environ['MSYSTEM'])
