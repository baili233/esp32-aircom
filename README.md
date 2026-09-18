# AirCom — ESP32-S3 无线串口终端

把 ESP32-S3 变成一个无线串口工具：UART1 的数据通过 WiFi 双向透传到浏览器里的
终端风格网页，串口参数与 WiFi 配置都能在网页上修改并掉电保存。

## 硬件

| 项目 | 说明 |
|---|---|
| 芯片 | ESP32-S3（4MB flash，无需 PSRAM） |
| 串口 | UART1：**GPIO17 = TX**，**GPIO18 = RX** |
| 日志 | 默认控制台（UART0 / USB-Serial-JTAG） |

接法：目标设备的 TX → GPIO18(RX)，目标设备的 RX → GPIO17(TX)，共地。

## 使用

1. 烧录后设备默认发出热点 `AirCom-XXXX`（XXXX 为 MAC 后四位），密码 `aircom123`。
2. 手机/电脑连接该热点，浏览器打开 <http://192.168.4.1/>。
3. 页面即终端：键盘输入直接进串口，串口输出实时上屏。
4. ⚙ 设置 里可改波特率/数据位/校验/停止位（立即生效 + NVS 保存）、
   切换 WiFi 模式（AP / STA / AP+STA）、扫描并加入路由器、重启设备。

## 构建（ESP-IDF v5.3+，本仓库按 v5.5 验证）

```bash
idf.py set-target esp32s3
idf.py build
idf.py flash monitor
```

Windows 下在 ESP-IDF CMD 环境中执行即可。

## API

| 端点 | 说明 |
|---|---|
| `GET /` | 终端网页（内嵌在固件里，无外部依赖） |
| `GET /api/status` | WiFi/串口/内存/丢帧等状态 |
| `GET /api/config` · `POST /api/uart` · `POST /api/wifi` | 配置读写 |
| `POST/GET /api/wifi/scan` | 非阻塞扫描 |
| `POST /api/reboot` | 重启 |
| `WS /ws` | 二进制帧 = 串口数据；文本帧 = JSON 控制（`{"t":"hello"}` / `{"t":"ping"}`） |

## 说明与限制

- 多个浏览器同时连接时共享同一条串口流（广播），所有输入都会写进串口。
- 高波特率（≥ 460800）为"尽力而为"：浏览器或网络跟不上时设备端会丢帧，
  状态栏 tooltip 里可以看到丢帧/溢出计数。
- AP+STA 模式下，STA 关联到路由器后热点会自动跟随路由器信道，切换瞬间
  热点客户端会有几百毫秒卡顿，属正常现象；STA 连接失败时状态栏会给出原因。
- WiFi 密码明文存于 NVS（内网工具，未开 flash encryption）。
- ANSI 支持：SGR 颜色/加粗/下划线/反显、清屏清行、光标移动；其余序列安全吞掉。
