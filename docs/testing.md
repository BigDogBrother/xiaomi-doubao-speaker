# 验证

## 不需要真实设备的回归测试

在 Linux 上安装 `cmake g++ libspdlog-dev python3`，然后：

```bash
cmake -S test/edge -B .build/tests
cmake --build .build/tests --parallel 2
ctest --test-dir .build/tests --output-on-failure
pip install -r tools/requirements.txt
python -m unittest discover -s test/edge -p 'test_*.py' -v
python tools/check_public_tree.py
```

回归覆盖：租约持有与心跳失败、超过原会话时限仍保持有效、独立看门狗失联恢复、忙闲切换、原生日志缺失/轮转、录音缓冲和回调分离、固定 Vivi 2.0 提示音协议及错误脱敏。

## 已做的实机检查

OH2P / 1.62.2：真实连续三分钟对话、人工插话、无自打断、退出后恢复原生小爱、整机重启后自动启动及缺少尚未创建的 instruction 日志时继续运行。API 另检查过 132 秒连接和六次固定语音生成。

“豆包同学”三米处仍有漏唤醒，不将这些测试解释为远场稳定性或长期稳定性保证。原生音乐和闹钟在豆包会话中的所有抢占边界未完整覆盖。

修改语音逻辑后，应在实际设备重新检查唤醒、第一句、长对话、插话、自回声、静默退出、原生天气/闹钟/米家，以及重启和故障恢复。本仓库不提交私人设备日志、账号或录音作为测试附件。
