import glob, zipfile, re, sys, io

sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8')

files = glob.glob('思路/*.docx')
target = None
for f in files:
    if '初步修改' in f:
        target = f
        break
if not target:
    target = files[0]

print('OPENING:', target)

z = zipfile.ZipFile(target)
xml = z.read('word/document.xml').decode('utf-8', errors='ignore')

# Split into paragraphs, extract <w:t> runs per paragraph
paras = re.split(r'<w:p[ >]', xml)
out = []
for p in paras:
    texts = re.findall(r'<w:t[^>]*>(.*?)</w:t>', p, re.S)
    txt = ''.join(texts)
    txt = txt.replace('&amp;', '&').replace('&lt;', '<').replace('&gt;', '>').replace('&quot;', '"').replace('&apos;', "'")
    if txt.strip():
        out.append(txt)

with open('scripts/_docx_text.txt', 'w', encoding='utf-8') as f:
    f.write('FILE: ' + target + '\n' + '=' * 60 + '\n\n')
    for i, p in enumerate(out):
        f.write(f'[{i:03d}] {p}\n')
    f.write('\nTOTAL_PARAS=%d\n' % len(out))
    f.write('TOTAL_CHARS=%d\n' % sum(len(p) for p in out))

print('WROTE', len(out), 'paragraphs,', sum(len(p) for p in out), 'chars')
