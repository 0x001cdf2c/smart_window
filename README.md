# Smart Window — ESP32-P4 语音智能窗帘

基于 ESP32-P4-WIFI6-DEV-KIT (Waveshare) 的离线语音识别智能窗帘系统。

## 硬件
- **MCU:** ESP32-P4-WIFI6-DEV-KIT
- **Codec:** ES8311 (I2S 立体声，16kHz)
- **麦克风:** 模拟 MEMS 麦克风

## 语音功能
| 唤醒词 | 说明 |
|--------|------|
| 你好乐鑫 | WakeNet9 |
| 嗨 ESP | WakeNet9 |

| 命令词 | 拼音 |
|--------|------|
| 打开窗帘 | da kai chuang lian |
| 关闭窗帘 | guan bi chuang lian |
| 停止 | ting zhi |
| 打开灯光 | da kai deng guang |
| 关闭灯光 | guan bi deng guang |

## 构建 & 烧录

```bash
# 构建
python build_n_flash.py build

# 烧录 (COM8)
python build_n_flash.py flash

# 重新配置 CMake 后构建
python build_n_flash.py build reconf
```

## 依赖
- ESP-IDF v5.5.4
- [espressif/es8311](https://components.espressif.com/components/espressif/es8311) ^1.0.0
- ESP-SR v2.x (WakeNet9 + MultiNet7)

## 引脚
| 功能 | GPIO |
|------|------|
| I2S MCLK | 13 |
| I2S BCK | 12 |
| I2S WS | 10 |
| I2S DIN | 11 |
| I2C SDA | 7 |
| I2C SCL | 8 |
| PA CTRL | 53 |
