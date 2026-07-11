# ESP32 AIoT Cloud Server

Render 云端中转服务，用于：

- ESP32 上传温湿度、光照和本地报警状态
- 任意手机打开公网网页查看数据
- 手机修改阈值和控制命令
- 点击网页按钮即时调用火山方舟大模型做中文风险分析

## 本地运行

```powershell
cd C:\Users\55363\Desktop\物联网数据大赛\esp32s3_aiot_competition\cloud_server
python -m pip install -r requirements.txt
python -m uvicorn app:app --reload --host 0.0.0.0 --port 8000
```

浏览器访问：

```text
http://127.0.0.1:8000
```

## Render 部署

1. 把整个 `esp32s3_aiot_competition` 工程推到 GitHub。
2. Render 新建 `Web Service`。
3. Root Directory 填：

```text
cloud_server
```

4. Build Command：

```text
pip install -r requirements.txt
```

5. Start Command：

```text
uvicorn app:app --host 0.0.0.0 --port $PORT
```

部署完成后，Render 会给一个公网地址，例如：

```text
https://esp32-aiot-cloud.onrender.com
```

这个地址后续要写回 ESP32 代码，二维码也会显示它。

## 火山方舟环境变量

在 Render 的 `Environment` 页面添加：

```text
ARK_API_KEY=你的火山方舟 API Key
ARK_MODEL=你的模型或推理接入点名称
ARK_BASE_URL=https://ark.cn-beijing.volces.com/api/v3
```

`ARK_BASE_URL` 不填也会使用上面的默认值。如果你拿到的是完整 `chat/completions` 地址，也可以额外设置 `ARK_ENDPOINT` 覆盖默认地址。未配置 `ARK_API_KEY` 或 `ARK_MODEL` 时，网页的“AI 风险分析”按钮会使用本地阈值规则兜底，方便先演示云端分析入口。
