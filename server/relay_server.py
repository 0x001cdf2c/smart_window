"""
WebSocket 消息中继服务器
- ESP32 设备注册并发送消息 → 转发给绑定的 Web UI
- Web UI 发送消息 → 转发给绑定的 ESP32 设备
- 同时提供 HTTP 静态文件服务 (Web UI)
"""

import asyncio
import json
import time
import websockets
from http.server import HTTPServer, SimpleHTTPRequestHandler
from pathlib import Path
import os
import threading

# ============================================================
# 配置
# ============================================================
WS_PORT = 8765
HTTP_PORT = 8766
WEBUI_DIR = Path(__file__).parent.parent / "webui"

# ============================================================
# 数据结构
# ============================================================
devices: dict[str, any] = {}
clients: dict[str, set] = {}
ws_to_device: dict[any, str] = {}

# ============================================================
# 日志
# ============================================================
def log(tag: str, msg: str):
    t = time.strftime("%H:%M:%S")
    print(f"[{t}] [{tag}] {msg}")

# ============================================================
# HTTP 静态文件服务 (运行在独立线程)
# ============================================================
def run_http():
    os.chdir(str(WEBUI_DIR))
    server = HTTPServer(("0.0.0.0", HTTP_PORT), SimpleHTTPRequestHandler)
    log("HTTP", f"Web UI 已启动 http://0.0.0.0:{HTTP_PORT}")
    server.serve_forever()

# ============================================================
# WebSocket 消息处理
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
            if not device_id:
                await ws.send(json.dumps({"type": "error", "payload": {"msg": "missing device_id"}}))
                continue

            role = msg.get("payload", {}).get("role", "device")

            if role == "device":
                devices[device_id] = ws
                ws_to_device[ws] = device_id
                clients.setdefault(device_id, set())
                log("DEVICE", f"上线: {device_id}")
                await ws.send(json.dumps({"type": "registered", "payload": {"device_id": device_id}}))

            elif role == "client":
                clients.setdefault(device_id, set()).add(ws)
                ws_to_device[ws] = device_id
                log("CLIENT", f"绑定设备: {device_id} ({len(clients[device_id])} 个客户端)")
                await ws.send(json.dumps({"type": "registered", "payload": {"device_id": device_id, "role": "client"}}))

                dev = devices.get(device_id)
                if dev:
                    await dev.send(json.dumps({"type": "client_online", "payload": {}}))

            else:
                await ws.send(json.dumps({"type": "error", "payload": {"msg": f"unknown role: {role}"}}))

        elif msg_type in ("send", "forward"):
            dev_id = ws_to_device.get(ws)
            if not dev_id:
                await ws.send(json.dumps({"type": "error", "payload": {"msg": "not registered"}}))
                continue

            payload = msg.get("payload", {})

            if role == "device" or (role is None and ws == devices.get(dev_id)):
                cs = clients.get(dev_id, set())
                fwd = json.dumps({"type": "forward", "payload": payload})
                for c in cs.copy():
                    try:
                        await c.send(fwd)
                    except:
                        cs.discard(c)
                log("FWD", f"设备→客户端 [{dev_id}]: {payload.get('type', '?')} → {len(cs)} 个客户端")
            else:
                dev = devices.get(dev_id)
                if dev:
                    fwd = json.dumps({"type": "forward", "payload": payload})
                    await dev.send(fwd)
                    log("FWD", f"客户端→设备 [{dev_id}]: {payload.get('type', '?')}")
                else:
                    await ws.send(json.dumps({"type": "error", "payload": {"msg": "device offline"}}))

        elif msg_type == "ping":
            await ws.send(json.dumps({"type": "pong", "payload": {}}))

        else:
            await ws.send(json.dumps({"type": "error", "payload": {"msg": f"unknown type: {msg_type}"}}))

    dev_id = ws_to_device.pop(ws, None)
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
            log("CLIENT", f"断开: {dev_id} (剩余 {len(clients.get(dev_id, set()))} 个)")
            dev = devices.get(dev_id)
            if dev:
                await dev.send(json.dumps({"type": "client_offline", "payload": {}}))


async def run_ws():
    log("WS", f"WebSocket 服务 ws://0.0.0.0:{WS_PORT}")
    async with websockets.serve(handle_connection, "0.0.0.0", WS_PORT):
        await asyncio.Future()

async def main():
    http_thread = threading.Thread(target=run_http, daemon=True)
    http_thread.start()
    await run_ws()

if __name__ == "__main__":
    asyncio.run(main())
