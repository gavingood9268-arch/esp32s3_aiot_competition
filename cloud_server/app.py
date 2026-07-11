import os
import time
from typing import Any

import requests
from dotenv import load_dotenv
from fastapi import FastAPI, Request
from fastapi.responses import HTMLResponse, JSONResponse

load_dotenv()

app = FastAPI(title="ESP32 AIoT Cloud")

DEVICE_ID = os.getenv("DEVICE_ID", "esp32s3-env-001")
ARK_API_KEY = os.getenv("ARK_API_KEY", "")
ARK_ENDPOINT = os.getenv("ARK_ENDPOINT", "")

state: dict[str, Any] = {
    "device_id": DEVICE_ID,
    "online": False,
    "updated_at": 0,
    "temperature": None,
    "humidity": None,
    "light_level": None,
    "light_raw": None,
    "local_alarm": False,
    "risk": "UNKNOWN",
    "risk_detail": "等待 ESP32 上传第一组数据。",
}

control: dict[str, Any] = {
    "temp_threshold": 30.0,
    "humi_threshold": 80.0,
    "light_threshold": 3950,
    "buzzer_enabled": True,
    "led_enabled": True,
    "manual_alarm": False,
    "command_id": 0,
}

ai_result: dict[str, Any] = {
    "status": "NOT_RUN",
    "level": 0,
    "summary": "尚未触发 AI 分析。",
    "advice": "请先等待 ESP32 上传数据，然后点击“AI 风险分析”。",
    "updated_at": 0,
}


def now_ms() -> int:
    return int(time.time() * 1000)


def seconds_since(ts: int) -> int:
    if not ts:
        return 999999
    return int(time.time() - ts)


def compute_local_risk(payload: dict[str, Any]) -> tuple[str, str]:
    issues: list[str] = []
    temp = payload.get("temperature")
    humi = payload.get("humidity")
    light_raw = payload.get("light_raw")

    if isinstance(temp, (int, float)) and temp > control["temp_threshold"]:
        issues.append(f"温度过高（{temp:.1f}°C > {control['temp_threshold']:.1f}°C）")
    if isinstance(humi, (int, float)) and humi > control["humi_threshold"]:
        issues.append(f"湿度过高（{humi:.1f}% > {control['humi_threshold']:.1f}%）")
    if isinstance(light_raw, (int, float)) and light_raw > control["light_threshold"]:
        issues.append(f"光照过强（RAW {int(light_raw)} > {int(control['light_threshold'])}）")

    if not issues:
        return "NORMAL", "当前温度、湿度和光照均处于设定阈值范围内。"
    return "WARN", "；".join(issues)


def html_page() -> str:
    return """<!doctype html>
<html lang="zh-CN">
<head>
  <meta charset="utf-8" />
  <meta name="viewport" content="width=device-width, initial-scale=1" />
  <title>ESP32 AIoT 环境风险终端</title>
  <style>
    :root {
      color-scheme: light;
      --bg: #f4f7fb;
      --panel: #ffffff;
      --ink: #152033;
      --muted: #667085;
      --line: #d7deea;
      --ok: #11845b;
      --warn: #c2410c;
      --bad: #b42318;
      --blue: #2563eb;
    }
    * { box-sizing: border-box; }
    body {
      margin: 0;
      font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif;
      background: var(--bg);
      color: var(--ink);
    }
    main {
      width: min(960px, calc(100vw - 28px));
      margin: 18px auto 32px;
    }
    header {
      display: flex;
      justify-content: space-between;
      align-items: flex-start;
      gap: 12px;
      margin-bottom: 14px;
    }
    h1 { font-size: 22px; margin: 0 0 6px; }
    .sub { color: var(--muted); font-size: 13px; }
    .pill {
      border: 1px solid var(--line);
      background: var(--panel);
      border-radius: 999px;
      padding: 7px 11px;
      font-size: 13px;
      white-space: nowrap;
    }
    .grid {
      display: grid;
      grid-template-columns: repeat(4, 1fr);
      gap: 10px;
    }
    .card {
      background: var(--panel);
      border: 1px solid var(--line);
      border-radius: 8px;
      padding: 14px;
      box-shadow: 0 1px 2px rgba(16, 24, 40, 0.04);
    }
    .metric-label { color: var(--muted); font-size: 12px; margin-bottom: 8px; }
    .metric-value { font-size: 28px; font-weight: 700; line-height: 1.1; }
    .status-ok { color: var(--ok); }
    .status-warn { color: var(--warn); }
    .status-bad { color: var(--bad); }
    .span-2 { grid-column: span 2; }
    .span-4 { grid-column: span 4; }
    label { display: block; color: var(--muted); font-size: 12px; margin-bottom: 5px; }
    input {
      width: 100%;
      border: 1px solid var(--line);
      border-radius: 6px;
      padding: 9px 10px;
      font-size: 15px;
    }
    input[type="checkbox"] {
      width: auto;
      transform: scale(1.2);
      margin-right: 8px;
    }
    button {
      border: 0;
      border-radius: 6px;
      background: var(--blue);
      color: white;
      padding: 10px 13px;
      font-weight: 650;
      cursor: pointer;
    }
    .btn-row { display: flex; gap: 8px; flex-wrap: wrap; }
    pre {
      white-space: pre-wrap;
      word-break: break-word;
      margin: 0;
      color: var(--muted);
      font-size: 12px;
    }
    @media (max-width: 720px) {
      .grid { grid-template-columns: repeat(2, 1fr); }
      .span-4, .span-2 { grid-column: span 2; }
    }
  </style>
</head>
<body>
  <main>
    <header>
      <div>
        <h1>ESP32 AIoT 环境风险终端</h1>
        <div class="sub">公网网页控制台 · 数据来自 ESP32-S3</div>
      </div>
      <div id="online" class="pill">加载中</div>
    </header>

    <section class="grid">
      <div class="card"><div class="metric-label">温度</div><div id="temp" class="metric-value">--</div></div>
      <div class="card"><div class="metric-label">湿度</div><div id="humi" class="metric-value">--</div></div>
      <div class="card"><div class="metric-label">光照指数</div><div id="light" class="metric-value">--</div></div>
      <div class="card"><div class="metric-label">风险状态</div><div id="risk" class="metric-value">--</div></div>

      <div class="card span-2">
        <div class="metric-label">本地风险说明</div>
        <div id="riskDetail">--</div>
      </div>
      <div class="card span-2">
        <div class="metric-label">AI 分析</div>
        <div id="aiSummary">--</div>
        <div class="sub" id="aiAdvice"></div>
      </div>

      <div class="card span-4">
        <h2 style="font-size:16px;margin:0 0 12px">阈值与控制</h2>
        <div class="grid">
          <div><label>温度阈值</label><input id="tempTh" type="number" step="0.1"></div>
          <div><label>湿度阈值</label><input id="humiTh" type="number" step="0.1"></div>
          <div><label>光照 RAW 阈值</label><input id="lightTh" type="number" step="1"></div>
          <div>
            <label>手动报警测试</label>
            <div style="padding-top:8px"><input id="manualAlarm" type="checkbox"><span class="sub">强制触发报警，用于测试蜂鸣器/LED/云端下发</span></div>
          </div>
        </div>
        <div class="btn-row" style="margin-top:12px">
          <button onclick="saveControl()">保存控制</button>
          <button onclick="runAi()">AI 风险分析</button>
        </div>
      </div>

      <div class="card span-4">
        <div class="metric-label">原始状态</div>
        <pre id="raw">--</pre>
      </div>
    </section>
  </main>

  <script>
    async function getJson(url, options) {
      const res = await fetch(url, options);
      return await res.json();
    }

    function clsByRisk(risk) {
      if (risk === "NORMAL") return "status-ok";
      if (risk === "DANGER") return "status-bad";
      return "status-warn";
    }

    function riskLabel(risk) {
      if (risk === "NORMAL") return "正常";
      if (risk === "DANGER") return "危险";
      if (risk === "WARN") return "预警";
      return "等待";
    }

    function setInputValueIfIdle(id, value) {
      const el = document.getElementById(id);
      if (document.activeElement !== el) el.value = value;
    }

    function setCheckboxIfIdle(id, checked) {
      const el = document.getElementById(id);
      if (document.activeElement !== el) el.checked = checked;
    }

    async function refresh() {
      const data = await getJson("/api/status");
      const s = data.state;
      const c = data.control;
      const ai = data.ai_result;
      const age = s.updated_at ? Math.round(Date.now() / 1000 - s.updated_at) : 999999;

      document.getElementById("online").textContent = s.online ? `在线 · ${age} 秒前更新` : "离线";
      document.getElementById("temp").textContent = s.temperature == null ? "--" : `${s.temperature.toFixed(1)}°C`;
      document.getElementById("humi").textContent = s.humidity == null ? "--" : `${s.humidity.toFixed(1)}%`;
      document.getElementById("light").textContent = s.light_level == null ? "--" : `${s.light_level}%`;
      const riskEl = document.getElementById("risk");
      riskEl.textContent = riskLabel(s.risk);
      riskEl.className = "metric-value " + clsByRisk(s.risk);
      document.getElementById("riskDetail").textContent = s.risk_detail;
      document.getElementById("aiSummary").textContent = ai.summary;
      document.getElementById("aiAdvice").textContent = ai.advice;
      setInputValueIfIdle("tempTh", c.temp_threshold);
      setInputValueIfIdle("humiTh", c.humi_threshold);
      setInputValueIfIdle("lightTh", c.light_threshold);
      setCheckboxIfIdle("manualAlarm", c.manual_alarm);
      document.getElementById("raw").textContent = JSON.stringify(data, null, 2);
    }

    async function saveControl() {
      await getJson("/api/control", {
        method: "POST",
        headers: {"Content-Type": "application/json"},
        body: JSON.stringify({
          temp_threshold: Number(document.getElementById("tempTh").value),
          humi_threshold: Number(document.getElementById("humiTh").value),
          light_threshold: Number(document.getElementById("lightTh").value),
          manual_alarm: document.getElementById("manualAlarm").checked
        })
      });
      await refresh();
    }

    async function runAi() {
      await getJson("/api/ai", {method: "POST"});
      await refresh();
    }

    refresh();
    setInterval(refresh, 2000);
  </script>
</body>
</html>"""


@app.get("/", response_class=HTMLResponse)
def index() -> str:
    return html_page()


@app.get("/health")
def health() -> dict[str, Any]:
    return {"ok": True, "time": int(time.time())}


@app.post("/api/update")
async def update_device(request: Request) -> dict[str, Any]:
    payload = await request.json()
    state.update(
        {
            "device_id": payload.get("device_id", DEVICE_ID),
            "online": True,
            "updated_at": int(time.time()),
            "temperature": payload.get("temperature"),
            "humidity": payload.get("humidity"),
            "light_level": payload.get("light_level"),
            "light_raw": payload.get("light_raw"),
            "local_alarm": bool(payload.get("local_alarm", False)),
        }
    )
    risk, detail = compute_local_risk(state)
    state["risk"] = "WARN" if control.get("manual_alarm") else risk
    state["risk_detail"] = "手动报警测试已开启：云端正在强制下发报警指令。" if control.get("manual_alarm") else detail
    return {"ok": True, "control": control, "ai_result": ai_result}


@app.get("/api/status")
def status() -> dict[str, Any]:
    state["online"] = seconds_since(state["updated_at"]) <= 15
    return {"state": state, "control": control, "ai_result": ai_result}


@app.post("/api/control")
async def set_control(request: Request) -> dict[str, Any]:
    payload = await request.json()
    for key in ["temp_threshold", "humi_threshold", "light_threshold"]:
        if key in payload:
            control[key] = payload[key]
    for key in ["buzzer_enabled", "led_enabled", "manual_alarm"]:
        if key in payload:
            control[key] = bool(payload[key])
    control["command_id"] = int(control["command_id"]) + 1
    return {"ok": True, "control": control}


@app.post("/api/ai")
def run_ai() -> JSONResponse:
    if not state.get("updated_at"):
        ai_result.update(
            {
                "status": "NO_DATA",
                "level": 0,
                "summary": "暂无设备数据。",
                "advice": "请先给 ESP32 上电并等待第一组传感器数据上传。",
                "updated_at": int(time.time()),
            }
        )
        return JSONResponse(ai_result)

    if not ARK_API_KEY or not ARK_ENDPOINT:
        ai_result.update(
            {
                "status": "STUB",
                "level": 1 if state["risk"] == "NORMAL" else 2,
                "summary": "本地规则分析完成。",
                "advice": "火山引擎大模型尚未配置。当前结果来自本地阈值规则，可用于先演示“云端分析”入口；后续接入火山 API 后会替换为大模型建议。",
                "updated_at": int(time.time()),
            }
        )
        return JSONResponse(ai_result)

    prompt = (
        "你是一个 AIoT 环境风险评估助手。"
        "请根据 ESP32 上传的温度、湿度、光照数据和阈值，给出中文风险等级、原因和控制建议。"
        "回答要简短，适合展示在竞赛演示网页上。"
        f"传感器数据：{state}。阈值与控制状态：{control}。"
    )
    try:
        response = requests.post(
            ARK_ENDPOINT,
            headers={"Authorization": f"Bearer {ARK_API_KEY}", "Content-Type": "application/json"},
            json={
                "messages": [
                    {"role": "system", "content": "你负责评估 ESP32 AIoT 环境监测终端的风险，并给出中文控制建议。"},
                    {"role": "user", "content": prompt},
                ]
            },
            timeout=20,
        )
        response.raise_for_status()
        content = response.text[:500]
        ai_result.update(
            {
                "status": "OK",
                "level": 1 if state["risk"] == "NORMAL" else 2,
                "summary": "AI 分析完成。",
                "advice": content,
                "updated_at": int(time.time()),
            }
        )
    except Exception as exc:
        ai_result.update(
            {
                "status": "ERROR",
                "level": 0,
                "summary": "AI 请求失败。",
                "advice": f"请检查火山引擎 API Key、Endpoint 和 Render 环境变量。错误信息：{exc}",
                "updated_at": int(time.time()),
            }
        )
    return JSONResponse(ai_result)
