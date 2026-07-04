import subprocess, os, sys

env = os.environ.copy()
env.pop('MSYSTEM', None)

# Find cmake and ninja - check common locations
idf_path = 'C:/Espressif/v5.5.4/esp-idf'
idf_tools = 'C:/Users/28145/.espressif/tools'
build_dir = 'C:/Users/28145/Desktop/IOT/code/Net/build'

# Let's check what tools exist
for root, dirs, files in os.walk(idf_tools):
    for f in files:
        if f in ('cmake.exe', 'ninja.exe'):
            print(os.path.join(root, f))
