"""Fail a public source check without printing any matched sensitive value."""
import ipaddress
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
PATTERNS = {
    'github-token': re.compile(rb'(?:gh[pousr]_[A-Za-z0-9]{20,}|github_pat_[A-Za-z0-9_]{20,})'),
    'private-key': re.compile(rb'(?m)^-----BEGIN (?:RSA |EC |OPENSSH |ENCRYPTED )?PRIVATE KEY-----'),
    'personal-email': re.compile(rb'[A-Za-z0-9._%+-]+@(?:qq|163|126|gmail|outlook|hotmail)\.[A-Za-z]{2,}'),
    'windows-user-path': re.compile(rb'[A-Za-z]:\\Users\\[A-Za-z0-9_-]+'),
}
IPV4 = re.compile(rb'\b(?:\d{1,3}\.){3}\d{1,3}\b')
ALLOWED_MODEL_VERSIONS = {'1.2.6.1'}
FORBIDDEN_PARTS = {'.secrets', '.runtime', '.venv', '__pycache__', 'node_modules', 'models'}
FORBIDDEN_SUFFIXES = {'.onnx', '.pcm', '.wav', '.log', '.key', '.pem', '.so', '.a', '.zip', '.exe'}

def main():
    data = subprocess.check_output(['git', 'ls-files', '-z'], cwd=ROOT)
    paths = [Path(x.decode('utf-8')) for x in data.split(b'\0') if x]
    if not paths:
        print('No tracked source files to check.');return 1
    findings = []
    for relative in paths:
        if set(relative.parts) & FORBIDDEN_PARTS or relative.name in {'.env', 'config.ini'} or relative.suffix in FORBIDDEN_SUFFIXES:
            findings.append((relative, 'private-or-binary-path'));continue
        payload = (ROOT/relative).read_bytes()
        if b'\0' in payload:
            findings.append((relative, 'binary-content'));continue
        for kind, pattern in PATTERNS.items():
            if pattern.search(payload):findings.append((relative, kind))
        for match in IPV4.finditer(payload):
            value = match.group().decode('ascii')
            if value in ALLOWED_MODEL_VERSIONS:continue
            try:address = ipaddress.ip_address(value)
            except ValueError:continue
            if not address.is_loopback:findings.append((relative, 'literal-network-address'));break
    for path, kind in findings:print(f'FAIL {path.as_posix()}: {kind}')
    if findings:return 1
    print(f'Public source check passed: {len(paths)} files; no matched secret values printed.')
    return 0

if __name__ == '__main__':sys.exit(main())
