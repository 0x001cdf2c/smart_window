#!/usr/bin/env python3
"""
prepare_nlp_model.py — 生成本地 NLP 分类器模型 (fastText 字符 n-gram)

用法: python scripts/prepare_nlp_model.py

输出: main/local_nlp_model.h (嵌入 C 头文件, ~100KB)

依赖: pip install numpy scikit-learn
"""

import re
import json
import struct
import hashlib
from collections import defaultdict
import numpy as np

# ── 训练数据 ──
# 中文命令例句 → 意图标签
TRAINING_DATA = [
    # ═══ 开窗 ═══
    ("打开窗户", "open"), ("打开窗帘", "open"), ("开窗", "open"),
    ("把窗户打开", "open"), ("开一下窗", "open"), ("开开窗", "open"),
    ("开窗透透气", "open"), ("通风", "open"), ("开窗通通风", "open"),
    ("打开一下窗帘", "open"), ("帮我开窗", "open"), ("太闷了开窗", "open"),
    ("开窗换气", "open"), ("开窗吧", "open"), ("有点热开窗", "open"),
    ("打开吧", "open"), ("透透气", "open"), ("通通风", "open"),
    ("把窗打开", "open"), ("我要开窗", "open"), ("请开窗", "open"),
    ("开下窗", "open"), ("窗户打开", "open"), ("热的开窗", "open"),
    ("闷死了开窗", "open"), ("开窗凉快", "open"), ("屋里太热", "open"),
    ("开窗散散热", "open"), ("打开窗户透透气", "open"),

    # ═══ 关窗 ═══
    ("关闭窗户", "close"), ("关闭窗帘", "close"), ("关窗", "close"),
    ("把窗户关上", "close"), ("关一下窗", "close"), ("关上窗", "close"),
    ("关窗遮光", "close"), ("太晒了关窗", "close"), ("太亮了关窗", "close"),
    ("光线太强关窗", "close"), ("帮我关窗", "close"), ("关起来吧", "close"),
    ("关窗吧", "close"), ("外面太亮关窗", "close"), ("反光关窗", "close"),
    ("晒死了关窗", "close"), ("太刺眼关窗", "close"), ("把帘子拉上", "close"),
    ("关上吧", "close"), ("我要关窗", "close"), ("请关窗", "close"),
    ("窗户关上", "close"), ("太阳太大了关窗", "close"), ("关窗防晒", "close"),
    ("遮阳", "close"), ("光线刺眼关窗", "close"), ("太热了关窗", "close"),

    # ═══ 停止 ═══
    ("停止", "stop"), ("停", "stop"), ("暂停", "stop"),
    ("停下来", "stop"), ("停一下", "stop"), ("先停", "stop"),
    ("别动了", "stop"), ("不要动了", "stop"), ("取消", "stop"),

    # ═══ 定时 ═══
    ("下午三点开窗", "timer"), ("早上八点打开窗帘", "timer"),
    ("晚上六点关上", "timer"), ("帮我定时下午两点打开", "timer"),
    ("三点开窗", "timer"), ("八点关窗", "timer"), ("定时一个小时以后关", "timer"),
    ("十分钟后开", "timer"), ("半小时后打开", "timer"), ("中午十二点关窗", "timer"),
    ("上午九点开窗", "timer"), ("帮我定个时下午三点开", "timer"),

    # ═══ 闲聊 ═══
    ("你好", "chat"), ("谢谢", "chat"), ("今天天气怎么样", "chat"),
    ("你是谁", "chat"), ("现在几点了", "chat"), ("晚安", "chat"),
    ("早上好", "chat"), ("今天适合开窗吗", "chat"), ("你在干嘛", "chat"),
    ("辛苦啦", "chat"), ("我回来了", "chat"), ("我出门了", "chat"),
]

# ── n-gram 特征提取 ──
def extract_char_ngrams(text, n_min=1, n_max=2):
    """从中文文本提取字符级 n-gram 哈希"""
    # 将中文按字符切分 (UTF-8)
    chars = list(text)
    ngrams = set()

    # 原始文本的 unigram/bigram
    for n in range(n_min, n_max + 1):
        for i in range(len(chars) - n + 1):
            ngrams.add(''.join(chars[i:i+n]))

    # 字节级 n-gram (捕获 UTF-8 内部特征, 对应 C 端实现)
    text_bytes = text.encode('utf-8')
    for n in range(1, 4):
        for i in range(len(text_bytes) - n + 1):
            ngrams.add('b' + text_bytes[i:i+n].hex())

    return ngrams

def ngram_hash(ngram_str):
    """FNV-1a hash (32-bit), 与 C 端一致"""
    h = 0x811C9DC5  # FNV offset basis
    for ch in ngram_str:
        h ^= ord(ch) if isinstance(ch, str) else ch
        h = (h * 0x01000193) & 0xFFFFFFFF
    return h

# ── 特征选择 ──
def build_vocab(texts, min_df=2, max_features=3000):
    """构建 n-gram 词汇表"""
    doc_freq = defaultdict(int)
    for text in texts:
        ngrams = extract_char_ngrams(text)
        for ng in ngrams:
            doc_freq[ng] += 1

    # 过滤低频
    filtered = {ng: freq for ng, freq in doc_freq.items() if freq >= min_df}

    # 按频率排序, 取 Top-N
    sorted_ngrams = sorted(filtered.items(), key=lambda x: -x[1])[:max_features]
    vocab = {ng: i for i, (ng, _) in enumerate(sorted_ngrams)}

    return vocab

# ── 训练线性分类器 (SGD + hinge loss) ──
def train_classifier(texts, labels, vocab, num_classes, lr=0.1, epochs=20):
    num_features = len(vocab)
    W = np.zeros((num_features, num_classes), dtype=np.float32)
    b = np.zeros(num_classes, dtype=np.float32)

    label_to_idx = {"open": 0, "close": 1, "stop": 2, "timer": 3, "chat": 4}

    for epoch in range(epochs):
        indices = np.random.permutation(len(texts))
        total_loss = 0
        correct = 0
        for idx in indices:
            text = texts[idx]
            y_true = label_to_idx[labels[idx]]

            # 特征向量
            ngrams = extract_char_ngrams(text)
            feats = np.zeros(num_features, dtype=np.float32)
            for ng in ngrams:
                if ng in vocab:
                    feats[vocab[ng]] += 1.0

            # 归一化
            norm = np.linalg.norm(feats)
            if norm > 0:
                feats /= norm

            # 得分 + hinge loss
            scores = W.T @ feats + b
            y_pred = np.argmax(scores)

            if y_pred == y_true:
                correct += 1

            # SGD update (hinge loss gradient)
            for c in range(num_classes):
                if c == y_true:
                    margin = 1.0 - scores[c]
                else:
                    margin = 1.0 + scores[c]

                if margin > 0:
                    grad = -1.0 if c == y_true else 1.0
                    W[:, c] -= lr * grad * feats
                    b[c] -= lr * grad

            total_loss += max(0, 1.0 - scores[y_true] + max(
                scores[c] for c in range(num_classes) if c != y_true
            ))

        acc = correct / len(texts)
        print(f"  epoch {epoch+1:2d}/{epochs}: loss={total_loss:.2f} acc={acc:.3f}")

    return W, b

# ── 评估 ──
def evaluate(texts, labels, vocab, W, b):
    label_to_idx = {"open": 0, "close": 1, "stop": 2, "timer": 3, "chat": 4}
    idx_to_label = {v: k for k, v in label_to_idx.items()}

    correct = 0
    results_by_class = defaultdict(lambda: {"correct": 0, "total": 0})

    for text, true_label in zip(texts, labels):
        ngrams = extract_char_ngrams(text)
        feats = np.zeros(len(vocab), dtype=np.float32)
        for ng in ngrams:
            if ng in vocab:
                feats[vocab[ng]] += 1.0

        norm = np.linalg.norm(feats)
        if norm > 0:
            feats /= norm

        scores = W.T @ feats + b
        y_pred = np.argmax(scores)
        pred_label = idx_to_label[y_pred]

        results_by_class[true_label]["total"] += 1
        if pred_label == true_label:
            results_by_class[true_label]["correct"] += 1
            correct += 1

    print(f"\n  总体准确率: {correct}/{len(texts)} = {correct/len(texts):.1%}")
    for cls in sorted(results_by_class):
        r = results_by_class[cls]
        print(f"  {cls:6s}: {r['correct']}/{r['total']} = {r['correct']/r['total']:.1%}")

# ── 导出 C 头文件 ──
def export_c_header(vocab, W, b, path):
    num_features = len(vocab)
    num_classes = W.shape[1]

    lines = []
    lines.append("/* Auto-generated by prepare_nlp_model.py — DO NOT EDIT */")
    lines.append(f"/* Features: {num_features}, Classes: {num_classes} */")
    lines.append("")
    lines.append("#pragma once")
    lines.append("")
    lines.append(f"#define NLP_NUM_FEATURES  {num_features}")
    lines.append(f"#define NLP_NUM_CLASSES   {num_classes}")
    lines.append("")

    # 权重矩阵 (row-major: feature * num_classes + class)
    lines.append("static const float NLP_WEIGHTS[] = {")
    for i in range(num_features):
        row = ", ".join(f"{W[i, c]:.6f}f" for c in range(num_classes))
        lines.append(f"    {row},")
    lines.append("};")
    lines.append("")

    # 偏置
    lines.append("static const float NLP_CLASS_BIAS[] = {")
    lines.append("    " + ", ".join(f"{b[c]:.6f}f" for c in range(num_classes)))
    lines.append("};")
    lines.append("")

    # 类标签
    lines.append("static const char *NLP_CLASS_NAMES[] = {")
    lines.append('    "open", "close", "stop", "timer", "chat"')
    lines.append("};")

    with open(path, 'w', encoding='utf-8') as f:
        f.write('\n'.join(lines) + '\n')

    print(f"\n  模型已导出: {path}")
    print(f"  权重: {num_features} × {num_classes} = {num_features * num_classes * 4 / 1024:.0f} KB")

# ── 主流程 ──
def main():
    import sys, os

    texts = [d[0] for d in TRAINING_DATA]
    labels = [d[1] for d in TRAINING_DATA]

    print(f"训练数据: {len(texts)} 条")

    # 打乱 + 8:2 分割
    indices = np.random.RandomState(42).permutation(len(texts))
    split = int(len(texts) * 0.8)
    train_idx = indices[:split]
    test_idx = indices[split:]

    train_texts = [texts[i] for i in train_idx]
    train_labels = [labels[i] for i in train_idx]
    test_texts = [texts[i] for i in test_idx]
    test_labels = [labels[i] for i in test_idx]

    print(f"训练集: {len(train_texts)}, 测试集: {len(test_texts)}")

    # 构建词汇表
    vocab = build_vocab(train_texts, min_df=2, max_features=3000)
    print(f"词汇表大小: {len(vocab)}")

    # 训练
    print("训练线性分类器...")
    W, b = train_classifier(train_texts, train_labels, vocab, num_classes=5)

    # 评估
    print("\n测试集评估:")
    evaluate(test_texts, test_labels, vocab, W, b)

    print("\n全量评估:")
    evaluate(texts, labels, vocab, W, b)

    # 导出
    out_path = os.path.join(os.path.dirname(__file__), "..", "main", "local_nlp_model.h")
    export_c_header(vocab, W, b, out_path)

    print("\n完成! 将 local_nlp_model.h 放入 main/ 目录即可启用 ML 模式。")

if __name__ == "__main__":
    main()
