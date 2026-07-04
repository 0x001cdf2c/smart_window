import subprocess
import os

# Remove MSYSTEM from environment to bypass ESP-IDF MSys check
env = os.environ.copy()
env.pop('MSYSTEM', None)
env['IDF_PATH'] = 'C:/Espressif/v5.5.4/esp-idf'
env['PATH'] = 'C:/Users/28145/.espressif/python_env/idf5.5_py3.13_env/Scripts;' + env.get('PATH', '')

result = subprocess.run(
    [
        'C:/Users/28145/.espressif/python_env/idf5.5_py3.13_env/Scripts/python.exe',
        'C:/Espressif/v5.5.4/esp-idf/tools/idf.py',
        '-p', 'COM8', 'flash'
    ],
    capture_output=True,
    text=True,
    cwd='C:/Users/28145/Desktop/IOT/code/Net',
    timeout=300,
    env=env
)
print('STDOUT:', result.stdout)
print('STDERR:', result.stderr)
print('RC:', result.returncode)
