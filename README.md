# ESP32-S3 AIoT Competition Project

这是从 `重点代码.txt` 整理出来的 VSCode / PlatformIO 工程副本，原始代码未被修改。

## 打开方式

1. 安装 VSCode。
2. 安装 PlatformIO IDE 扩展。
3. 在 VSCode 中选择 `File -> Open Folder`。
4. 打开这个文件夹：

```text
C:\Users\55363\Desktop\物联网数据大赛\esp32s3_aiot_competition
```

## 当前工程状态

当前代码实现了：

- ESP32-S3 主控
- SHT30 温湿度采集
- 光敏模拟量采集
- ST7789 屏幕显示
- 单按键页面切换
- 蜂鸣器阈值报警
- ESP32 Wi-Fi AP 热点
- Web 页面查看数据和配置阈值

## 竞赛下一步改造方向

后续需要新增：

- Wi-Fi STA 联网模式
- HTTP POST 上传环境 JSON
- 中转服务器接口
- 阿里云百炼 qwen3.7-max 大模型调用
- AI 风险等级显示
- 大模型下发阈值和报警策略
- 本地断网兜底逻辑
