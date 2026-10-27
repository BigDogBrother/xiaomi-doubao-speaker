import os,subprocess,tempfile,time
from pathlib import Path
source=(Path(__file__).resolve().parents[2]/'device/doubao-lease.sh').read_text(encoding='utf-8')
with tempfile.TemporaryDirectory(prefix='edge-lease-test-') as name:
 root=Path(name);lease=root/'lease';helper=root/'lease.sh';mute=root/'mute';clock=root/'clock';clock.write_text('0\n')
 text=source.replace('/tmp/doubao-voice-lease.sh',str(helper)).replace('/tmp/doubao-voice-lease',str(lease)).replace('/tmp/mipns/mute',str(mute)).replace('/tmp/doubao-voice-watchdog.log',str(root/'watch.log')).replace('now_s() { cut -d. -f1 /proc/uptime; }','now_s() { cat "$EDGE_TEST_CLOCK"; }').replace('sleep 1\n','sleep 0.05\n')
 helper.write_text(text);helper.chmod(0o700)
 ubus=root/'ubus';ubus.write_text('#!/bin/sh\ncase "$*" in *\'"event":7\'*) touch "$EDGE_TEST_MUTE";; *\'"event":8\'*) rm -f "$EDGE_TEST_MUTE";; *) exit 1;; esac\n');ubus.chmod(0o700)
 env=dict(os.environ,PATH=str(root)+':'+os.environ['PATH'],EDGE_TEST_CLOCK=str(clock),EDGE_TEST_MUTE=str(mute));token='a'*32
 def run(action):subprocess.run(['/bin/sh',str(helper),action,token],check=True,env=env,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
 try:
  run('begin');assert mute.exists()
  (lease/'heartbeat').write_text('200\n');clock.write_text('200\n');run('heartbeat');time.sleep(.12)
  assert mute.exists() and lease.exists(), 'progressing session was hard-capped'
  clock.write_text('208\n')
  deadline=time.monotonic()+3
  while mute.exists() and time.monotonic()<deadline:time.sleep(.05)
  assert not mute.exists() and not lease.exists(), 'stale heartbeat did not restore native'
 finally:
  if lease.exists():run('end')
print('lease_long_session_and_stale_heartbeat_ok')
