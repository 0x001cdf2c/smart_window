"""
将 WAV 文件转换为 C 语言 PCM 数组, 用于语音回复。
用法: python wav_to_pcm.py audio.wav
输出可以直接替换 voice_reply.c 中的占位数组。
"""
import wave
import sys
import os

def wav_to_c_array(wav_path, var_name):
    with wave.open(wav_path, 'rb') as w:
        assert w.getnchannels() == 1, "必须是单声道 WAV"
        assert w.getsampwidth() == 2, "必须是 16-bit WAV"
        assert w.getframerate() == 16000, "必须是 16000 Hz 采样率"
        n = w.getnframes()
        data = w.readframes(n)

    samples = []
    for i in range(0, len(data), 2):
        val = int.from_bytes(data[i:i+2], 'little', signed=True)
        samples.append(str(val))

    lines = []
    line = []
    for s in samples:
        line.append(s)
        if len(line) >= 16:
            lines.append('    ' + ','.join(line) + ',')
            line = []
    if line:
        lines.append('    ' + ','.join(line) + ',')

    return (
        f'/* WAV: {os.path.basename(wav_path)} */\n'
        f'const int16_t {var_name}[] = {{\n'
        + '\n'.join(lines) + '\n'
        + '};\n'
        f'const size_t {var_name}_len = {len(samples)};\n'
    )

if __name__ == '__main__':
    if len(sys.argv) < 2:
        print("用法: python wav_to_pcm.py <file1.wav> [file2.wav ...]")
        print("要求: 16kHz, 16bit, mono WAV")
        sys.exit(1)

    # WAV 文件名 → C 数组名映射
    name_map = {
        'wo_zai':      'audio_wo_zai',
        'shou_dao':    'audio_shou_dao',
        'da_kai':      'audio_da_kai',
        'guan_bi':     'audio_guan_bi',
        'ting_zhi':    'audio_ting_zhi',
        'da_kai_deng': 'audio_da_kai_deng',
        'guan_bi_deng':'audio_guan_bi_deng',
    }

    for wav_path in sys.argv[1:]:
        stem = os.path.splitext(os.path.basename(wav_path))[0]
        var = name_map.get(stem, f'audio_{stem}')
        try:
            c_code = wav_to_c_array(wav_path, var)
            print(c_code)
            print()
        except Exception as e:
            print(f"// ERROR: {wav_path}: {e}", file=sys.stderr)
