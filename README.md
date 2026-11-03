# 小米 OH2P × 豆包实时语音（音箱端）

在 **Xiaomi 智能音箱 Pro OH2P / 固件 1.62.2** 上运行的实验性 C++ 客户端。参考 [xiaoai-plus](https://github.com/kslr/xiaoai-plus)，直接连接火山豆包实时语音 API，保留小爱原生功能，不依赖电脑或自建云端转发服务。

- **小爱同学**：原生小米语音系统，负责米家、天气、闹钟、音乐。
- **豆包同学**：本地唤醒后进入豆包会话，使用 **Vivi 2.0**，支持连续对话和讲话时插话。
- 两套助手会话互斥；豆包播完后安静约 6 秒退出，也可说“退出豆包”。
- 持续有交互或播放进展时，不因会话总时长达到两分钟而强制退出。
- 进程失联由音箱上的独立租约看门狗恢复原生唤醒。
- 30 分钟内可接续历史会话；支持在本机保存少量明确的爱好和习惯，详见[记忆说明](docs/memory.md)。私人背景需在设备上单独配置。

> 当前是已实机验证的个人实验版本。**三米处的“豆包同学”仍可能需要重复唤醒**，不能视为具有原生小爱相同的远场识别率。仅验证上述型号/固件；需要事先具有合法可用的 root/SSH 权限。本仓库不提供刷机固件或账号凭据。

## 接口与运行位置

接口：`wss://openspeech.bytedance.com/api/v3/duplex/realtime/dialogue`。模型 `1.2.6.1`，音色 `zh_female_vv_uranus_bigtts`（Vivi 2.0），输入 16 kHz 单声道 PCM，输出 24 kHz S16 PCM。

音箱运行客户端、唤醒模型、收放音及回声消除；豆包模型仍由火山服务提供。因此仍需要互联网、有效 API Key，并产生接口用量。待机不向豆包上传麦克风；日志不保存录音、识别文本或认证响应。

## 开始之前

1. 已能 SSH 登录兼容的 OH2P，且原生小爱功能正常。
2. 音箱存在 `arecord`、`aplay`、`ubus` 和系统 CA 证书。
3. 已开通对应豆包实时语音 API。本项目不使用小米账号密码或方舟文字模型接口。
4. Linux/Docker 用于构建；Python 3.12 用于管理和生成固定开场提示音。

## 构建

```bash
git clone https://github.com/BigDogBrother/xiaomi-doubao-speaker.git
cd xiaomi-doubao-speaker
docker build --output type=local,dest=dist .
```

输出 `dist/doubao-edge-runtime.tar.gz`，包含客户端、模型、独立 ARM 运行库、示例配置及启动脚本。**不包含 API Key 和开场提示音**。构建会下载上游源码、模型和 Debian ARM 包，具体版本见 [构建说明](docs/build.md)。模型及依赖各自受上游许可约束。

## 配置与安装

完整步骤见 [安装说明](docs/install.md)。基本顺序：

1. 将运行包解压到音箱 `/data/doubao-edge`。
2. 根据 `config.ini.example` 在音箱本地填写 API Key，并设置权限为 `600`。
3. 在电脑生成固定的 Vivi 2.0 提示音，并传至音箱 `vivi-wake.pcm`。
4. 先前台验证，再按需启用开机启动。

```bash
python -m venv .venv
. .venv/bin/activate
pip install -r tools/requirements.txt
# 在本机环境中设置 DOUBAO_API_KEY，避免将它写进共享命令、代码或提交。
python tools/make_wake_prompt.py --output .runtime/vivi-wake.pcm
```

该工具只生成固定的“我在，请说。”，不会录音。实际 API Key 的配置及输入由使用者在本机完成。

## 日常使用

说“豆包同学”，听到“我在，请说”后再提问。对话中可正常插话；说“退出豆包”，或等回答结束后安静约 6 秒，再使用“小爱同学”。豆包没有米家控制或闹钟工具权限，相关请求请使用小爱。

可选的局域网管理：将 `config/speaker.json.example` 复制为 `.secrets/speaker.json`，填写自己的音箱连接信息，并先用 SSH 确认主机指纹。管理脚本拒绝未知主机密钥。

```bash
python tools/manage.py status
python tools/manage.py stop
python tools/manage.py start
```

## 验证与限制

已验证：Vivi 2.0、连续三分钟真实对话、人工插话、退出后立即恢复小爱、整机重启后自动启动、原生日志尚未创建时保持就绪。接口另做过 132 秒连接和六次固定语音生成测试。

限制：

- 小型本地唤醒模型的远场成功率仍不稳定，可能受距离、噪声、发音影响。
- 不支持在一套助手当前会话中直接切入另一套；需先退出或等待静默结束。
- 60 秒没有有效会话/播放进展会作为故障结束；网络、服务端限流或异常仍可能中断回答。
- 播放缓存有上限；持续超长生成触及上限时会停止继续生成并保留已有播放。
- 原生音乐、闹钟在豆包会话中的抢占等所有边界并未全部验收，长期稳定性仍需观察。

测试方法见 [验证说明](docs/testing.md)，运行关系见 [设计说明](docs/design.md)。

## 代码结构

```text
device/edge/       C++ 实时语音、唤醒、音频、会话状态与启动脚本
device/            原生唤醒租约、独立 ALSA 路由、开机集成
tools/             构建、固定提示音生成、局域网管理、发布检查
test/edge/         不依赖真实音箱或 API Key 的回归测试
config/            仅含连接配置模板
docs/              构建、安装、设计和验证说明
```

## 许可

MIT；上游归属与依赖说明见 [NOTICE](NOTICE.md)。本项目不是小米或火山的官方产品，不包含小米固件、模型私有配置、个人账号或部署记录。
