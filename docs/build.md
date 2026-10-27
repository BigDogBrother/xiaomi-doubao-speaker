# 构建说明

推荐使用仓库 Dockerfile 的 Debian bookworm 容器。Windows 主机可用 Docker Desktop；不需要将编译器安装到音箱。GitHub Actions 会独立编译测试和 ARM 客户端。

## 固定版本

| 依赖 | 版本/来源 |
|---|---|
| nlohmann/json | v3.11.3 |
| IXWebSocket | v11.4.5 |
| spdlog | v1.15.1 |
| sherpa-onnx C++ API 和 ARM shared runtime | v1.12.25 |
| 唤醒模型 | sherpa-onnx-kws-zipformer-zh-en-3M-2025-12-20 |
| Silero VAD | 上游 asr-models 发布，SHA-256 `9e2449e1087496d8d4caba907f23e0bd3f78d91fa552479bb9c23ac09cbb1fd6` |
| OpenSSL / zlib / WebRTC AudioProcessing | Debian bookworm ARMHF 包 |

源码标签及模型版本被固定；Debian 安全更新和其发布包可能变化，因此不宣称逐字节可复现。构建包会生成 `SHA256SUMS`。

## ARM 运行库

设备固件的旧 glibc 无法直接加载所用 sherpa-onnx ARM 预编译库。运行包包含独立的 ARMHF glibc 及加载器：

```sh
/data/doubao-edge/lib/ld-linux-armhf.so.3 \
 --library-path /data/doubao-edge/lib \
 /data/doubao-edge/doubao-edge -c /data/doubao-edge/config.ini
```

它们只通过这个命令显式加载，不覆盖音箱系统库，也不改系统 `LD_LIBRARY_PATH`。启动脚本会先切换到运行目录，模型路径相对于该目录。

## 构建过程

`tools/build-armhf.sh` 下载依赖，使用 `arm-linux-gnueabihf-g++` 和 `device/edge/armv7-linux-gnueabihf.cmake` 交叉编译，组装 `dist/doubao-edge-runtime.tar.gz`。`.build/`、`dist/` 和下载物均被 Git 忽略。

只发布源码；不要把包含实际 `config.ini` 的个人运行目录直接打包分享。固定开场提示音用自己的 API Key 生成，之后放到设备本地。
