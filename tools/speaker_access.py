"""SSH access for developer management; private configuration stays outside Git."""
import json
from pathlib import Path
import paramiko

ROOT = Path(__file__).resolve().parents[1]

def connect(config_path=None):
    path = Path(config_path) if config_path else ROOT / '.secrets' / 'speaker.json'
    settings = json.loads(path.read_text(encoding='utf-8-sig'))
    client = paramiko.SSHClient()
    client.load_system_host_keys()
    known_hosts = ROOT / '.secrets' / 'known_hosts'
    if known_hosts.exists():
        client.load_host_keys(str(known_hosts))
    client.set_missing_host_key_policy(paramiko.RejectPolicy())
    kwargs = dict(hostname=settings['host'], port=int(settings.get('port', 22)),
                  username=settings.get('username', 'root'), timeout=5,
                  banner_timeout=5, auth_timeout=5, look_for_keys=True, allow_agent=True)
    if settings.get('password'):
        kwargs['password'] = settings['password']
    if settings.get('key_filename'):
        kwargs['key_filename'] = str(Path(settings['key_filename']).expanduser())
    client.connect(**kwargs)
    return client
