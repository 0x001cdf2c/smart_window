import subprocess, os, sys

env = os.environ.copy()
env.pop('MSYSTEM', None)
env['IDF_PATH'] = 'C:/Espressif/v5.5.4/esp-idf'
env['IDF_PYTHON_ENV_PATH'] = 'C:/Espressif/tools/python_env/idf5.5_py3.13_env'
env['IDF_TOOLS_PATH'] = 'C:/Espressif/tools'

extra_paths = [
    'C:/Espressif/tools/python_env/idf5.5_py3.13_env/Scripts',
    'C:/Espressif/tools/cmake/3.30.2/bin',
    'C:/Espressif/tools/ninja/1.12.1',
    'C:/Espressif/tools/riscv32-esp-elf/esp-14.2.0_20260121/riscv32-esp-elf/bin',
    'C:/Espressif/tools/ccache/4.9',
    'C:/Espressif/tools/idf-exe/1.0.3',
]
env['PATH'] = ';'.join(extra_paths) + ';' + env.get('PATH', '')

python = 'C:/Espressif/tools/python_env/idf5.5_py3.13_env/Scripts/python.exe'
idf_py = 'C:/Espressif/v5.5.4/esp-idf/tools/idf.py'
cwd = 'C:/Users/28145/Desktop/IOT/code/Net'

step = sys.argv[1] if len(sys.argv) > 1 else 'build'

# Allow re-running cmake by removing cache
if len(sys.argv) > 2 and sys.argv[2] == 'reconf':
    import shutil
    cmake_cache = 'C:/Users/28145/Desktop/IOT/code/Net/build/CMakeCache.txt'
    if os.path.exists(cmake_cache):
        os.remove(cmake_cache)

result = subprocess.run(
    [python, idf_py, step],
    capture_output=True, text=True, cwd=cwd, timeout=300, env=env
)
print('STDOUT:', result.stdout[-3000:])
print('STDERR:', result.stderr[-2000:])
print('RC:', result.returncode)
