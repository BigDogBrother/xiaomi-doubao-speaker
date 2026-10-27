"""Manage the on-speaker service. Does not install or change voice settings."""
import argparse
from speaker_access import connect

STATUS = '''if [ ! -f /data/doubao-edge/enabled ]; then
 echo DISABLED
else
 p=$(cat /tmp/doubao-edge-worker.pid 2>/dev/null || true)
 case "$p" in ''|*[!0-9]*) echo NOT_READY;; *)
  if kill -0 "$p" 2>/dev/null && [ "$p" = "$(cat /tmp/doubao-edge.ready 2>/dev/null)" ]; then echo READY; else echo NOT_READY; fi;;
 esac
fi'''
COMMANDS = {
    'status': STATUS,
    'start': 'touch /data/doubao-edge/enabled && sh /data/doubao-edge/start.sh && sleep 12; ' + STATUS,
    'stop': 'sh /data/doubao-edge/stop.sh',
}

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=COMMANDS)
    parser.add_argument('--config', help='Local SSH JSON configuration, never commit it')
    args = parser.parse_args()
    client = connect(args.config)
    try:
        _, out, err = client.exec_command(COMMANDS[args.action], timeout=40)
        result = out.read().decode(errors='replace')
        err.read()
        if out.channel.recv_exit_status() != 0:
            raise RuntimeError('speaker_command_failed')
        print(result.strip())
    finally:
        client.close()

if __name__ == '__main__':
    try:
        main()
    except Exception as error:
        print('Speaker management failed:', type(error).__name__)
        raise SystemExit(1)
