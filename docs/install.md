# 安装到已 root 的 OH2P

本说明只面向 OH2P / 1.62.2，且假设已拥有 SSH 权限。刷机、固件恢复和账号获取不在本仓库范围内。安装前先确认原生小爱、米家、天气和闹钟正常。

## 1. 安装运行包

将自己构建的 `dist/doubao-edge-runtime.tar.gz` 传到音箱，解压为 `/data/doubao-edge`。例如在 Linux/macOS 的 shell 中：

```bash
ssh root@SPEAKER_IP 'cat > /tmp/doubao-edge-runtime.tar.gz' < dist/doubao-edge-runtime.tar.gz
ssh root@SPEAKER_IP
```

然后在音箱中操作：

```sh
test ! -e /data/doubao-edge || { echo existing_installation_requires_backup; exit 1; }
mkdir -p /data/doubao-edge
cd /data/doubao-edge
tar -xzf /tmp/doubao-edge-runtime.tar.gz
chmod 700 *.sh doubao-edge
cp config.ini.example config.ini
chmod 600 config.ini
```

使用设备上的编辑器在 `config.ini` 中填写 API Key。不要将真实配置提交 Git、发到 issue 或放入公开附件。已有安装应先停止并保留自己的配置、提示音和原启动文件，再手动升级。

## 2. 固定 Vivi 2.0 开场提示音

在电脑的私有环境中设置 `DOUBAO_API_KEY`，安装 `tools/requirements.txt`，运行：

```bash
python tools/make_wake_prompt.py --output .runtime/vivi-wake.pcm
ssh root@SPEAKER_IP 'cat > /data/doubao-edge/vivi-wake.pcm && chmod 600 /data/doubao-edge/vivi-wake.pcm' < .runtime/vivi-wake.pcm
```

这些重定向示例是 Bash 语法，Windows 用户需使用支持二进制传输的 SSH 工具。不要通过文本编码转换来传 PCM 或 tar 包。

## 3. 前台验证

音箱中运行：

```sh
cd /data/doubao-edge
cp lease.sh /tmp/doubao-voice-lease.sh
chmod 700 /tmp/doubao-voice-lease.sh
./lib/ld-linux-armhf.so.3 --library-path /data/doubao-edge/lib ./doubao-edge -c config.ini --probe-api
./lib/ld-linux-armhf.so.3 --library-path /data/doubao-edge/lib ./doubao-edge -c config.ini
```

`--probe-api` 会调用付费接口生成固定测试语音，不会播放或录音。前台客户端只运行一份；验证结束按 Ctrl+C，再启用后台版本。不要同时运行其它接管麦克风/原生语音 socket 的项目。

## 4. 开机运行与停止

```sh
sh /data/doubao-edge/enable-autostart.sh
```

该脚本检查必要文件，备份已有 `/data/init.sh`，追加独立的 `DOUBAO EDGE` 标记段；保留已有其他启动内容，不停止原生语音服务。不要覆盖整个 `/data/init.sh`。

停止及关闭自启：

```sh
sh /data/doubao-edge/stop.sh
```

它移除本程序的 enabled 标记，结束自己的进程并等待原生唤醒恢复。再次启用可重新运行 `enable-autostart.sh`。确认原生恢复后，才手动移除启动标记段或程序文件。

## 5. 可选的电脑管理

复制 `config/speaker.json.example` 为 `.secrets/speaker.json`，填写自己的连接信息。建议使用 SSH Key。先通过自己的 SSH 客户端核对设备主机指纹；也可用下面的选项将确认后的记录保存到项目私有目录：

```bash
mkdir -p .secrets
ssh -o UserKnownHostsFile=.secrets/known_hosts root@SPEAKER_IP
python tools/manage.py status
```

`manage.py` 拒绝未知主机密钥，不会自动跳过指纹检查。电脑只用于管理，关机不影响音箱上的服务。
