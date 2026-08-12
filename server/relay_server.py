"""
Smart Blinds Server — WebSocket relay + HTTP web UI + weather push
Usage:
    python relay_server.py [--city 南京] [--weather-interval 1800]
"""

import asyncio
import json
import time
import os
import threading
from pathlib import Path
from http.server import HTTPServer, SimpleHTTPRequestHandler
from concurrent.futures import ThreadPoolExecutor

import websockets
import requests

# ============================================================
# 配置
# ============================================================
WS_PORT = 8765
HTTP_PORT = 8766
WEBUI_DIR = Path(__file__).parent.parent / "webui"
DEVICE_ID = "blinds_001"

# ── Weather ──
GEO_URL = "https://geocoding-api.open-meteo.com/v1/search"
WEATHER_URL = "https://api.open-meteo.com/v1/forecast"
WMO_CODES = {
    0: "晴", 1: "晴", 2: "多云", 3: "阴",
    45: "雾", 48: "雾凇", 51: "小雨", 53: "小雨", 55: "中雨",
    61: "小雨", 63: "中雨", 65: "大雨", 71: "小雪", 73: "中雪",
    75: "大雪", 77: "雪粒", 80: "阵雨", 81: "阵雨", 82: "强阵雨",
    85: "阵雪", 86: "阵雪", 95: "雷暴", 96: "冰雹", 99: "强冰雹",
}

# ── 逐小时天气摘要 (从 hourly 数据生成口语化播报) ──
def _build_hourly_summary(hourly: dict) -> str:
    """Generate a natural-language summary like '大部分时间晴, 下午3-5点有阵雨'."""
    if not hourly:
        return ""

    times = hourly.get("time", [])
    codes = hourly.get("weather_code", [])
    rain_probs = hourly.get("precipitation_probability", [])

    if not times or not codes:
        return ""

    # 统计主导天气
    code_count = {}
    for c in codes:
        code_count[c] = code_count.get(c, 0) + 1
    dominant_code = max(code_count, key=code_count.get)
    dominant_text = WMO_CODES.get(dominant_code, "多云")
    dominant_hours = code_count[dominant_code]
    total_hours = len(codes)

    parts = []

    # 主导天气描述
    if dominant_hours >= total_hours * 0.7:
        parts.append(f"全天大部分时间{dominant_text}")
    elif dominant_hours >= total_hours * 0.4:
        parts.append(f"大部分时间{dominant_text}")
    else:
        parts.append(f"天气{dominant_text}")

    # 降雨时段
    rain_slots = []
    for i, t in enumerate(times):
        prob = rain_probs[i] if i < len(rain_probs) else 0
        if prob >= 50:
            hour = t.split("T")[1][:2] if "T" in t else ""
            if hour:
                rain_slots.append((int(hour), prob))

    if rain_slots:
        # 合并连续降雨时段
        rain_slots.sort()
        merged = []
        start_h, start_p = rain_slots[0]
        end_h, max_p = start_h, start_p
        for i in range(1, len(rain_slots)):
            h, p = rain_slots[i]
            if h == end_h + 1:
                end_h = h
                max_p = max(max_p, p)
            else:
                merged.append((start_h, end_h, max_p))
                start_h, end_h, max_p = h, h, p
        merged.append((start_h, end_h, max_p))

        rain_strs = []
        for sh, eh, p in merged:
            if sh == eh:
                rain_strs.append(f"{sh}点")
            else:
                rain_strs.append(f"{sh}-{eh}点")
        parts.append(f"{'、'.join(rain_strs)}有{'雨' if rain_slots[0][1] >= 70 else '可能下雨'}")

    return "，".join(parts) if parts else dominant_text

# ── DeepSeek ──
DEEPSEEK_API_KEY = os.environ.get("DEEPSEEK_API_KEY", "sk-10e46871ed9545e9a644f1419a3208a7")
DEEPSEEK_API_URL = "https://api.deepseek.com/v1/chat/completions"
# ⚠️ 用户自行修改下面的提示词
SUGGESTION_PROMPT = """根据以下传感器和天气数据，给出约20字的穿搭和今日生活建议。
要求直接给出建议，不要前缀，不要解释，20字以内。

数据：
- 室内温度: {temp}°C
- 室内湿度: {humidity}%
- 室内光照: {light} lux
- 今日天气: {weather}
- 最高温: {high}°C / 最低温: {low}°C
- 降水概率: {rain_pct}%
- 风向风力: {wind}"""

# ============================================================
# 数据结构
# ============================================================
devices: dict[str, any] = {}
clients: dict[str, set] = {}
ws_to_device: dict[any, str] = {}
ws_to_role: dict[any, str] = {}

# ── 自适应模式云端增强 prompt ──
ADAPTIVE_PROMPT = """你是一个智能窗户的云端大脑。设备端已完成本地学习，给你以下数据：

1. 用户最近操作记录 (时间+角度+多少天前)
2. 本地预测的开窗计划 (基于时间桶+传感器偏移)
3. 当前室内传感器数据
4. 室外天气数据

请分析用户习惯，返回优化后的开窗计划。

返回纯JSON (不要markdown):
{
  "plan": [
    {"time": "08:00", "angle": 0},
    {"time": "12:00", "angle": 90}
  ],
  "reply": "口语化解释, 30字以内"
}

规则:
- plan最多4条, 按时间排序
- angle: 0=全开, 90=全关, 30-60=半开
- 考虑天气因素: 下雨天减少开窗时段, 高温天中午关窗遮阳
- 参考用户历史习惯: 相近时间的操作权重更高
- reply用口语气息解释你的决策 (呀、啦、嘛、呢)
"""

# ============================================================
# 全局
# ============================================================
weather_city: str = ""
latest_sensor_data: dict = {}
latest_weather_data: dict = {}

def log(tag: str, msg: str):
    t = time.strftime("%H:%M:%S")
    print(f"[{t}] [{tag}] {msg}")

# ============================================================
# ASR: 阿里云 NUI 语音识别 (免费 500次/天)
# NLP: kourichat gpt-4o-mini (意图理解+口语回复)
# ============================================================
# 阿里云 NUI
ALIBABA_AK_ID  = os.environ.get("ALIBABA_AK_ID", "LTAI5tAWbBNG1czQ2UWZj6LR")
ALIBABA_AK_SEC = os.environ.get("ALIBABA_AK_SEC", "q9eGtr2Y95nuMpQDnzpIw6H9rCd4eZ")
NUI_APPKEY     = os.environ.get("NUI_APPKEY", "F1cFX8KM7SWl1UBE")
# kourichat (OpenAI-compatible)
LLM_API_KEY  = os.environ.get("LLM_API_KEY", DEEPSEEK_API_KEY)
LLM_BASE_URL = os.environ.get("LLM_BASE_URL", "https://api.deepseek.com/v1")
LLM_MODEL    = os.environ.get("LLM_MODEL", "deepseek-v4-flash")

# Per-device audio accumulation
audio_buffers: dict[str, bytearray] = {}

VOICE_SYSTEM_PROMPT = """你是一个智能窗户语音助手。会收到一段用户语音，结合传感器和天气数据，判断意图并回复。

返回JSON (纯JSON, 不要markdown代码块):
{"action": "<open|close|chat|timer>", "reply": "<口语回复, 30字以内>"}

意图判断:
- 想开窗/透透气/太热/闷 → action="open"
- 想关窗/太吵/灰大/太亮 → action="close"
- 闲聊/问天气/问建议 → action="chat"
- 定时/帮我在X点开窗/关窗/X点提醒我开窗/X点关窗 → action="timer"
  需要额外返回 time="HH:MM"(24小时制) 和 cmd="open"/"close"
  例如: {"action":"timer","time":"12:00","cmd":"open","reply":"好的, 12点帮你开窗~"}

时间解析规则:
- "12点" → "12:00", "下午3点" → "15:00", "早上8点" → "08:00"
- "8点半" → "08:30", "下午2点半" → "14:30"
- "五分钟后" → 当前时间+5分钟(HH:MM格式)

reply: 口语气息, 像朋友聊天, 加点语气词(呀、啦、嘛、哦、呢、哈), 30字以内

★★★ 关键: "执行"模式 vs 自主模式 ★★★

1. 用户说了"执行"(如"执行,打开窗户"):
   → 必须忠实执行用户的命令 (action用用户想要的open/close)
   → reply中可附加一句天气建议, 但命令不能改
   → 例: {"action":"open","reply":"打开了~ 不过外面35度有点热哦"}

2. 用户没提"执行"(如"打开窗户"):
   → 根据天气/传感器数据自主判断是否执行
   → 下雨/雷暴/大风/沙尘暴/严重雾霾 → 建议别开窗, action="chat", reply解释原因
   → 外面比屋里热很多(>30°C且室内<28°C) → 建议别开窗
   → 天气合适 → 执行用户命令
   → 例(下雨): {"action":"chat","reply":"外面正下雨呢, 等雨停再开吧~"}
   → 例(太热): {"action":"chat","reply":"现在外面35度, 开着窗更热, 先别开啦"}
   → 例(合适): {"action":"open","reply":"天不错, 开窗透透气~"}

示例:
{"action": "open", "reply": "是有点闷, 开窗透透气~"}
{"action": "close", "reply": "关上啦, 安静多啦"}
{"action": "chat", "reply": "热呀! 现在32度, 小心中暑哦"}
{"action": "timer", "time": "12:00", "cmd": "open", "reply": "好的, 中午12点帮你开窗~"}
"""

# ============================================================
# 天气
# ============================================================
def fetch_weather(city_name: str) -> dict | None:
    """Synchronous weather fetch (runs in thread pool)."""
    try:
        geo = requests.get(GEO_URL, params={"name": city_name, "count": 1, "language": "zh"}, timeout=10)
        geo.raise_for_status()
        geo_data = geo.json()
        if not geo_data.get("results"):
            log("WEATHER", f"未找到城市: {city_name}")
            return None

        loc = geo_data["results"][0]
        lat, lon = loc["latitude"], loc["longitude"]

        w = requests.get(WEATHER_URL, params={
            "latitude": lat, "longitude": lon,
            "current": "temperature_2m,relative_humidity_2m,wind_speed_10m,wind_direction_10m",
            "daily": "weather_code,temperature_2m_max,temperature_2m_min,"
                     "precipitation_probability_max,wind_speed_10m_max",
            "hourly": "weather_code,precipitation_probability,precipitation,temperature_2m",
            "timezone": "auto", "forecast_days": 1,
        }, timeout=10)
        w.raise_for_status()
        w_data = w.json()

        daily = w_data.get("daily", {})
        current = w_data.get("current", {})
        hourly = w_data.get("hourly", {})
        daily_code = (daily.get("weather_code") or [0])[0]
        weather_text = WMO_CODES.get(daily_code, f"code{daily_code}")

        # 从逐小时数据生成详细天气摘要
        weather_detail = _build_hourly_summary(hourly)

        wind_dir = current.get("wind_direction_10m", 0)
        dirs = ["北", "东北", "东", "东南", "南", "西南", "西", "西北"]
        wind_dir_cn = dirs[round(wind_dir / 45) % 8]

        return {
            "city": loc.get("name", city_name),
            "weather": weather_text,
            "weather_detail": weather_detail,
            "high": (daily.get("temperature_2m_max") or [0])[0],
            "low": (daily.get("temperature_2m_min") or [0])[0],
            "rain_pct": (daily.get("precipitation_probability_max") or [0])[0],
            "wind": f"{wind_dir_cn}风 {current.get('wind_speed_10m', 0):.0f}级",
        }
    except Exception as e:
        log("WEATHER", f"获取失败: {e}")
        return None

def fetch_suggestion(sensor: dict, weather: dict) -> str | None:
    """Call DeepSeek API (runs in thread pool)."""
    try:
        prompt = SUGGESTION_PROMPT.format(
            temp=sensor.get("temp", "--"),
            humidity=sensor.get("humidity", "--"),
            light=sensor.get("light", "--"),
            weather=weather.get("weather", "--"),
            high=weather.get("high", "--"),
            low=weather.get("low", "--"),
            rain_pct=weather.get("rain_pct", "--"),
            wind=weather.get("wind", "--"),
        )
        resp = requests.post(DEEPSEEK_API_URL, json={
            "model": "deepseek-v4-pro",
            "messages": [
                {"role": "user", "content": prompt},
            ],
            "max_tokens": 80,
            "temperature": 0.7,
        }, headers={
            "Authorization": f"Bearer {DEEPSEEK_API_KEY}",
            "Content-Type": "application/json",
        }, timeout=15)
        resp.raise_for_status()
        data = resp.json()
        return data["choices"][0]["message"]["content"].strip()
    except Exception as e:
        log("DS", f"API调用失败: {e}")
        return None

# ── ASR: PCM → 阿里云 NUI → text (免费 500次/天) ──
import hmac
import hashlib
import base64
import uuid

_nls_token: str = ""
_nls_token_expire: float = 0.0

def _get_nls_token() -> str:
    """Get or refresh Alibaba Cloud NLS token."""
    global _nls_token, _nls_token_expire
    now = time.time()
    if _nls_token and now < _nls_token_expire:
        return _nls_token

    if not ALIBABA_AK_ID or not ALIBABA_AK_SEC:
        raise RuntimeError("缺少 ALIBABA_AK_ID / ALIBABA_AK_SEC 环境变量")

    # Step 1: request a token via HMAC-SHA1 signed request
    params = {
        "AccessKeyId": ALIBABA_AK_ID,
        "Action": "CreateToken",
        "Version": "2019-02-28",
        "Format": "JSON",
        "RegionId": "cn-shanghai",
        "Timestamp": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(now)),
        "SignatureMethod": "HMAC-SHA1",
        "SignatureVersion": "1.0",
        "SignatureNonce": str(uuid.uuid4()),
    }
    sorted_keys = sorted(params.keys())
    qs = "&".join(f"{k}={requests.utils.quote(params[k], safe='')}" for k in sorted_keys)
    sign_str = f"GET&{requests.utils.quote('/', safe='')}&{requests.utils.quote(qs, safe='')}"
    signature = base64.b64encode(
        hmac.new(f"{ALIBABA_AK_SEC}&".encode(), sign_str.encode(), hashlib.sha1).digest()
    ).decode()
    params["Signature"] = signature

    url = f"https://nls-meta.cn-shanghai.aliyuncs.com/pop/2019-02-28/tokens"
    resp = requests.get(url, params=params, timeout=10)
    resp.raise_for_status()
    data = resp.json()
    token_info = data.get("Token", {})
    _nls_token = token_info.get("Id", "")
    _nls_token_expire = now + token_info.get("ExpireTime", 3600) - 60
    log("NLS", f"Token 已获取 (过期: {time.strftime('%H:%M:%S', time.localtime(_nls_token_expire))})")
    return _nls_token

def fetch_asr(pcm_data: bytes, sample_rate: int = 16000) -> str | None:
    """Send raw PCM to Alibaba Cloud NUI one-sentence recognition (blocking)."""
    try:
        token = _get_nls_token()
        appkey = NUI_APPKEY
        if not appkey:
            log("ASR", "未配置 NUI_APPKEY, 跳过")
            return None

        url = f"https://nls-gateway-cn-shanghai.aliyuncs.com/stream/v1/asr"
        params = {
            "appkey": appkey,
            "format": "pcm",
            "sample_rate": str(sample_rate),
        }
        headers = {
            "X-NLS-Token": token,
            "Content-Type": "application/octet-stream",
        }
        resp = requests.post(url, params=params, headers=headers,
                             data=pcm_data, timeout=15)
        if resp.status_code != 200:
            log("ASR", f"API返回 {resp.status_code}: {resp.text[:300]}")
        resp.raise_for_status()
        result = resp.json()
        text = result.get("result", "").strip()
        if text:
            log("ASR", f"识别结果: {text}")
        else:
            log("ASR", f"无结果 (status={result.get('status')}, full={json.dumps(result, ensure_ascii=False)})")
        return text
    except Exception as e:
        log("ASR", f"识别失败: {e}")
        return None

# ── Fallback NLP: 云端失败时用关键词匹配 (保证必有语音回复) ──
def fallback_nlp(text: str) -> dict | None:
    """Simple keyword fallback when DeepSeek is unavailable.
    Returns None ONLY for timer commands that need time extraction."""
    has_open = any(w in text for w in ["打开", "开窗", "开开", "通风", "透气", "换气",
                                        "太闷", "闷死", "热死", "有点热", "凉快"])
    has_close = any(w in text for w in ["关闭", "关窗", "关上", "遮光", "遮阳",
                                         "防晒", "太晒", "太亮", "刺眼", "反光"])
    has_stop = any(w in text for w in ["停止", "暂停", "别动", "取消"])
    has_timer = any(w in text for w in ["点", "分", "半", "定时", "后", "分钟", "小时"])

    if has_timer and (has_open or has_close):
        return None  # timer needs time extraction, can't fallback
    if has_open:
        return {"action": "open", "reply": "好的，打开窗户~"}
    if has_close:
        return {"action": "close", "reply": "好的，关上窗户~"}
    if has_stop:
        return {"action": "close", "reply": "收到，已停止"}  # close action stops servo
    # 所有其他情况 (包括闲聊) → 至少给个语音回复
    return {"action": "chat", "reply": "嗯嗯，我在听~"}

# ── NLP: text + context → DeepSeek → intent + reply ──
def fetch_nlp(text: str, context: str) -> dict | None:
    """Send recognized text + sensor/weather context to gpt-4o-mini via kourichat (blocking).
    Returns {"action": "open"|"close"|"chat", "reply": "..."} or None."""
    try:
        user_msg = f"用户说: {text}\n\n{context}"
        resp = requests.post(f"{LLM_BASE_URL}/chat/completions", json={
            "model": LLM_MODEL,
            "messages": [
                {"role": "system", "content": VOICE_SYSTEM_PROMPT},
                {"role": "user", "content": user_msg},
            ],
            "max_tokens": 120,
            "temperature": 0.7,
        }, headers={
            "Authorization": f"Bearer {LLM_API_KEY}",
            "Content-Type": "application/json",
        }, timeout=15)
        resp.raise_for_status()
        data = resp.json()
        raw = data["choices"][0]["message"]["content"].strip()

        # Strip markdown code fences
        if raw.startswith("```"):
            raw = raw.split("\n", 1)[-1]
            if raw.endswith("```"):
                raw = raw[:-3]
            raw = raw.strip()

        try:
            result = json.loads(raw)
        except json.JSONDecodeError:
            log("NLP", f"非JSON回复: {raw}")
            return None
        log("NLP", f"意图={result.get('action')} 回复={result.get('reply')}")
        return result
    except requests.exceptions.HTTPError as e:
        log("NLP", f"HTTP错误 {e.response.status_code}: {e.response.text[:200]}")
        return None
    except Exception as e:
        log("NLP", f"失败: {e}")
        return None

# ── 自适应模式云端增强 ──
async def handle_adaptive_query(dev_id: str, data: dict):
    """设备本地学习完成 → 发送摘要到 DeepSeek → 返回增强计划."""
    # 构建天气上下文
    weather_ctx = ""
    if latest_weather_data:
        w = latest_weather_data
        detail = w.get('weather_detail', '')
        weather_ctx = (f"室外: {w.get('weather','--')}, {w.get('low','--')}~{w.get('high','--')}°C, "
                       f"降水{w.get('rain_pct','--')}%")
        if detail:
            weather_ctx += f", {detail}"

    user_msg = json.dumps({
        "用户操作记录": data.get("recent_ops", []),
        "本地预测计划": data.get("predictions", []),
        "室内传感器": f"温度{data.get('temp','--')}°C 湿度{data.get('humidity','--')}% 光照{data.get('light','--')}lux",
        "天气": weather_ctx,
    }, ensure_ascii=False, indent=2)

    log("ADAPTIVE", f"发送增强查询 (records={len(data.get('recent_ops',[]))}, "
                    f"predictions={len(data.get('predictions',[]))})")

    loop = asyncio.get_running_loop()
    with ThreadPoolExecutor(max_workers=1) as pool:
        result = await loop.run_in_executor(pool, fetch_adaptive, user_msg)

    if result:
        await send_to_device(dev_id, {"type": "adaptive_plan", **result})
    else:
        log("ADAPTIVE", "云端增强失败, 使用本地计划")

def fetch_adaptive(user_msg: str) -> dict | None:
    """Call DeepSeek to refine adaptive plan."""
    try:
        resp = requests.post(f"{LLM_BASE_URL}/chat/completions", json={
            "model": LLM_MODEL,
            "messages": [
                {"role": "system", "content": ADAPTIVE_PROMPT},
                {"role": "user", "content": user_msg},
            ],
            "max_tokens": 800,
            "temperature": 0.5,
        }, headers={
            "Authorization": f"Bearer {LLM_API_KEY}",
            "Content-Type": "application/json",
        }, timeout=20)
        resp.raise_for_status()
        data = resp.json()
        raw = data["choices"][0]["message"]["content"].strip()
        finish = data["choices"][0].get("finish_reason", "?")

        if not raw:
            log("ADAPTIVE", f"DeepSeek 返回空内容 (finish_reason={finish}, "
                f"usage={data.get('usage', {})})")
            return None

        if raw.startswith("```"):
            raw = raw.split("\n", 1)[-1]
            if raw.endswith("```"):
                raw = raw[:-3]
            raw = raw.strip()

        result = json.loads(raw)
        log("ADAPTIVE", f"云端计划: {result.get('plan', [])}")
        return result
    except json.JSONDecodeError as e:
        log("ADAPTIVE", f"JSON解析失败: {e} | raw={raw[:200]}")
        return None
    except Exception as e:
        log("ADAPTIVE", f"失败: {e}")
        return None

# ── Async handlers for ASR pipeline ──
async def handle_asr_audio(dev_id: str, inner: dict, device_ws):
    """Accumulate audio chunks, call ASR on end-of-speech."""
    buf = audio_buffers.setdefault(dev_id, bytearray())

    import base64
    b64 = inner.get("audio", "")
    try:
        chunk = base64.b64decode(b64)
        if chunk:
            buf.extend(chunk)
    except Exception as e:
        log("ASR", f"base64解码失败: {e}")

    if inner.get("is_end"):
        if len(buf) < 1600:  # < 50ms, too short
            log("ASR", f"音频太短 ({len(buf)} bytes), 跳过")
            audio_buffers.pop(dev_id, None)
            return

        pcm = bytes(buf)
        audio_buffers.pop(dev_id, None)

        loop = asyncio.get_running_loop()
        with ThreadPoolExecutor(max_workers=1) as pool:
            text = await loop.run_in_executor(pool, fetch_asr, pcm)

        if not text:
            await send_to_device(dev_id, {"type": "asr_result", "text": ""})
            return

        # Send ASR result to device
        await send_to_device(dev_id, {"type": "asr_result", "text": text})

async def handle_asr_text(dev_id: str, text: str, force_exec: bool, device_ws):
    """Combine text with sensor/weather context, call DeepSeek, return intent+reply."""
    if not text:
        return

    # Build context from latest data
    ctx_parts = []
    if latest_sensor_data:
        s = latest_sensor_data
        ctx_parts.append(
            f"室内: 温度{s.get('temp','--')}°C, "
            f"湿度{s.get('humidity','--')}%, "
            f"光照{s.get('light','--')} lux")
    if latest_weather_data:
        w = latest_weather_data
        detail = w.get('weather_detail', '')
        ctx_parts.append(
            f"天气: {w.get('weather','--')}, "
            f"{w.get('low','--')}~{w.get('high','--')}°C, "
            f"降水概率{w.get('rain_pct','--')}%"
            + (f", 详细: {detail}" if detail else ""))
    context = "\n".join(ctx_parts) if ctx_parts else "暂无传感器数据"
    if force_exec:
        context = "[用户说了\"执行\"——必须忠实执行用户命令, 只需在reply中附加天气建议]\n" + context

    loop = asyncio.get_running_loop()
    with ThreadPoolExecutor(max_workers=1) as pool:
        result = await loop.run_in_executor(pool, fetch_nlp, text, context)

    if not result:
        # 云端 NLP 失败 → fallback 关键词匹配
        result = fallback_nlp(text)
        if not result:
            return
        log("NLP", f"云端失败, fallback: action={result.get('action')}")

    action = result.get("action", "chat")
    reply = result.get("reply", "")

    if action in ("open", "close"):
        # Send command to device (device executes + speaks)
        await send_to_device(dev_id, {"type": "asr_command", "action": action, "reply": reply})
    elif action == "timer":
        # Voice timer: send time + cmd to device
        timer_time = result.get("time", "")
        timer_cmd = result.get("cmd", "open")
        await send_to_device(dev_id, {
            "type": "asr_command",
            "action": "timer",
            "time": timer_time,
            "cmd": timer_cmd,
            "reply": reply
        })
    else:
        # Just speak the reply
        await send_to_device(dev_id, {"type": "asr_reply", "text": reply})

async def send_to_device(dev_id: str, payload: dict):
    """Send a forward message to the device."""
    dev = devices.get(dev_id)
    if dev:
        try:
            await dev.send(json.dumps({
                "type": "forward",
                "payload": payload,
            }, ensure_ascii=False))
        except Exception as e:
            log("SEND", f"发送失败: {e}")

async def push_weather(city: str):
    """Fetch weather and push to device + all web clients."""
    loop = asyncio.get_running_loop()
    with ThreadPoolExecutor(max_workers=1) as pool:
        data = await loop.run_in_executor(pool, fetch_weather, city)

    if not data:
        return

    detail = data.get('weather_detail', '')
    log("WEATHER", f"{data['city']}: {data['weather']} {data['low']}~{data['high']}°C "
                   f"降水{data['rain_pct']}% {data['wind']}" +
                   (f" | {detail}" if detail else ""))

    # cache for suggestion
    latest_weather_data.clear()
    latest_weather_data.update(data)

    # push to device (if online)
    dev = devices.get(DEVICE_ID)
    if dev:
        try:
            await dev.send(json.dumps({
                "type": "forward",
                "payload": {"type": "weather", **data},
            }, ensure_ascii=False))
        except Exception as e:
            log("WEATHER", f"推送设备失败: {e}")
    else:
        log("WEATHER", "设备不在线, 仅推送到 Web 客户端")

    # push to all web clients (always)
    weather_msg = json.dumps({
        "type": "weather",
        "payload": {"type": "weather", **data},
    }, ensure_ascii=False)
    for c in clients.get(DEVICE_ID, set()).copy():
        try:
            await c.send(weather_msg)
        except:
            clients.get(DEVICE_ID, set()).discard(c)

    # auto-generate suggestion when both sensor + weather available
    if latest_sensor_data:
        asyncio.create_task(push_suggestion())

async def push_suggestion():
    """Generate suggestion and push to device + web clients."""
    if not latest_sensor_data or not latest_weather_data:
        log("DS", "缺少数据, 跳过")
        return

    loop = asyncio.get_running_loop()
    with ThreadPoolExecutor(max_workers=1) as pool:
        text = await loop.run_in_executor(
            pool, fetch_suggestion, dict(latest_sensor_data), dict(latest_weather_data))

    if not text:
        return

    log("DS", f"建议: {text}")

    # push to device
    dev = devices.get(DEVICE_ID)
    if dev:
        try:
            await dev.send(json.dumps({
                "type": "forward",
                "payload": {"type": "suggestion", "text": text},
            }, ensure_ascii=False))
        except Exception as e:
            log("DS", f"推送设备失败: {e}")

    # push to all web clients
    suggestion_msg = json.dumps({
        "type": "suggestion",
        "payload": {"type": "suggestion", "text": text},
    }, ensure_ascii=False)
    for c in clients.get(DEVICE_ID, set()).copy():
        try:
            await c.send(suggestion_msg)
        except:
            clients.get(DEVICE_ID, set()).discard(c)

async def weather_loop(city: str, interval: int):
    """Background task: fetch weather periodically."""
    while True:
        await push_weather(city)
        await asyncio.sleep(interval)

# ============================================================
# HTTP 静态文件服务
# ============================================================
def run_http():
    os.chdir(str(WEBUI_DIR))
    server = HTTPServer(("0.0.0.0", HTTP_PORT), SimpleHTTPRequestHandler)
    log("HTTP", f"Web UI → http://0.0.0.0:{HTTP_PORT}")
    server.serve_forever()

# ============================================================
# WebSocket
# ============================================================
async def handle_connection(ws):
    role = None
    device_id = None

    async for raw in ws:
        try:
            msg = json.loads(raw)
        except json.JSONDecodeError:
            await ws.send(json.dumps({"type": "error", "payload": {"msg": "invalid json"}}))
            continue

        msg_type = msg.get("type", "")

        if msg_type == "register":
            device_id = msg.get("payload", {}).get("device_id", "")
            role = msg.get("payload", {}).get("role", "device")

            if role == "device":
                devices[device_id] = ws
                ws_to_device[ws] = device_id
                ws_to_role[ws] = "device"
                clients.setdefault(device_id, set())
                log("DEVICE", f"上线: {device_id}")
                await ws.send(json.dumps({"type": "registered", "payload": {"device_id": device_id}}))
                # push weather on device connect
                if weather_city:
                    asyncio.create_task(push_weather(weather_city))

            elif role == "client":
                clients.setdefault(device_id, set()).add(ws)
                ws_to_device[ws] = device_id
                ws_to_role[ws] = "client"
                log("CLIENT", f"加入 [{device_id}] ({len(clients[device_id])} 在线)")
                await ws.send(json.dumps({"type": "registered", "payload": {"device_id": device_id, "role": "client"}}))
                dev = devices.get(device_id)
                if dev:
                    try:
                        await dev.send(json.dumps({"type": "client_online", "payload": {}}))
                    except:
                        pass

        elif msg_type in ("send", "forward"):
            dev_id = ws_to_device.get(ws)
            if not dev_id:
                continue

            payload = msg.get("payload", {})

            # parse inner payload (may be string or dict)
            inner = payload if isinstance(payload, dict) else {}
            if isinstance(payload, str):
                try:
                    inner = json.loads(payload)
                except:
                    pass

            # cache sensor data from device
            if ws_to_role.get(ws) == "device" and isinstance(inner, dict):
                if any(k in inner for k in ("temp", "humidity", "light")):
                    latest_sensor_data.clear()
                    latest_sensor_data.update(inner)

            # handle queries
            if inner.get("type") == "weather_query" and weather_city:
                log("WEATHER", "收到天气查询请求")
                asyncio.create_task(push_weather(weather_city))
            elif inner.get("type") == "suggestion_query":
                log("DS", "收到建议查询请求")
                asyncio.create_task(push_suggestion())

            # ── ASR audio streaming (device → server) ──
            elif inner.get("type") == "asr_audio" and ws_to_role.get(ws) == "device":
                asyncio.create_task(handle_asr_audio(dev_id, inner, ws))

            # ── NLP text parsing (device → server → DeepSeek) ──
            elif inner.get("type") == "asr_text" and ws_to_role.get(ws) == "device":
                asyncio.create_task(handle_asr_text(dev_id, inner.get("text", ""),
                                                    inner.get("force_execute", False), ws))

            # ── 自适应模式云端增强 (device → server → DeepSeek) ──
            elif inner.get("type") == "adaptive_query" and ws_to_role.get(ws) == "device":
                asyncio.create_task(handle_adaptive_query(dev_id, inner))

            if ws_to_role.get(ws) == "device":
                # device → all clients
                fwd = json.dumps({"type": "forward", "payload": payload}, ensure_ascii=False)
                for c in clients.get(dev_id, set()).copy():
                    try:
                        await c.send(fwd)
                    except:
                        clients.get(dev_id, set()).discard(c)

            else:
                # client → device
                dev = devices.get(dev_id)
                if dev:
                    fwd = json.dumps({"type": "forward", "payload": payload}, ensure_ascii=False)
                    try:
                        await dev.send(fwd)
                    except:
                        pass

        elif msg_type == "ping":
            await ws.send(json.dumps({"type": "pong", "payload": {}}))

    # ── disconnected ──
    dev_id = ws_to_device.pop(ws, None)
    ws_to_role.pop(ws, None)
    if dev_id:
        if ws == devices.get(dev_id):
            del devices[dev_id]
            log("DEVICE", f"离线: {dev_id}")
            for c in clients.get(dev_id, set()).copy():
                try:
                    await c.send(json.dumps({"type": "device_offline", "payload": {}}))
                except:
                    pass
        else:
            clients.get(dev_id, set()).discard(ws)
            log("CLIENT", f"断开 [{dev_id}] ({len(clients.get(dev_id, set()))} 在线)")
            dev = devices.get(dev_id)
            if dev:
                try:
                    await dev.send(json.dumps({"type": "client_offline", "payload": {}}))
                except:
                    pass

# ============================================================
# Main
# ============================================================
async def main(city: str, weather_interval: int):
    global weather_city
    weather_city = city

    http_thread = threading.Thread(target=run_http, daemon=True)
    http_thread.start()

    log("WS", f"WebSocket → ws://0.0.0.0:{WS_PORT}")
    if city:
        asyncio.create_task(weather_loop(city, weather_interval))
        log("WEATHER", f"已启用 城市={city} 间隔={weather_interval}s")

    async with websockets.serve(handle_connection, "0.0.0.0", WS_PORT):
        await asyncio.Future()

if __name__ == "__main__":
    import argparse
    parser = argparse.ArgumentParser(description="Smart Blinds Server")
    parser.add_argument("--city", default="南京", help="天气城市 (默认: 南京)")
    parser.add_argument("--weather-interval", type=int, default=1800,
                        help="天气推送间隔/秒 (默认: 1800)")
    args = parser.parse_args()
    asyncio.run(main(args.city, args.weather_interval))
