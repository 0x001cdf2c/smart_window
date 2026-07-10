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
            "timezone": "auto", "forecast_days": 1,
        }, timeout=10)
        w.raise_for_status()
        w_data = w.json()

        daily = w_data.get("daily", {})
        current = w_data.get("current", {})
        daily_code = (daily.get("weather_code") or [0])[0]
        weather_text = WMO_CODES.get(daily_code, f"code{daily_code}")

        wind_dir = current.get("wind_direction_10m", 0)
        dirs = ["北", "东北", "东", "东南", "南", "西南", "西", "西北"]
        wind_dir_cn = dirs[round(wind_dir / 45) % 8]

        return {
            "city": loc.get("name", city_name),
            "weather": weather_text,
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
            "model": "deepseek-chat",
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

async def push_weather(city: str):
    """Fetch weather and push to device + all web clients."""
    loop = asyncio.get_running_loop()
    with ThreadPoolExecutor(max_workers=1) as pool:
        data = await loop.run_in_executor(pool, fetch_weather, city)

    if not data:
        return

    log("WEATHER", f"{data['city']}: {data['weather']} {data['low']}~{data['high']}°C "
                   f"降水{data['rain_pct']}% {data['wind']}")

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
