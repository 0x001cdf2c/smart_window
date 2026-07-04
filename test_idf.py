import subprocess
import os

os.environ['IDF_PATH'] = 'C:/Espressif/v5.5.4/esp-idf'
os.environ['PATH'] = 'C:/Users/28145/.espressif/python_env/idf5.5_py3.13_env/Scripts;' + os.environ.get('PATH', '')

result = subprocess.run(
    [
        'C:/Users/28145/.espressif/python_env/idf5.5_py3.13_env/Scripts/python.exe',
        'C:/Espressif/v5.5.4/esp-idf/tools/idf.py',
        '--version'
    ],
    capture_output=True,
    text=True,
    cwd='C:/Users/28145/Desktop/IOT/code/Net',
    timeout=30
)
print('STDOUT:', repr(result.stdout))
print('STDERR:', repr(result.stderr))
print('RC:', result.returncode)
