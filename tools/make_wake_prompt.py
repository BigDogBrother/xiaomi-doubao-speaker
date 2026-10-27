"""Create only the fixed Vivi 2.0 greeting; never records microphone audio."""
import argparse
import base64
import json
import os
from pathlib import Path
import time
import uuid
from websockets.sync.client import connect

ENDPOINT = 'wss://openspeech.bytedance.com/api/v3/duplex/realtime/dialogue'
VOICE = 'zh_female_vv_uranus_bigtts'
PROMPT = '我在，请说。'

class PromptError(RuntimeError):
    pass

def synthesize(api_key, connector=connect):
    if not api_key.strip():
        raise PromptError('api_key_missing')
    chunks = []
    total = 0
    deadline = time.monotonic() + 20
    try:
        with connector(ENDPOINT, additional_headers={'X-Api-Key': api_key.strip()},
                       open_timeout=10, close_timeout=3, max_size=2*1024*1024) as socket:
            def send(kind, **fields):
                socket.send(json.dumps(dict(type=kind, event_id=str(uuid.uuid4()), **fields)))
            send('session.create', session={
                'model': '1.2.6.1',
                'audio': {'input': {'format': {'type': 'pcm', 'rate': 16000}},
                          'output': {'format': {'type': 'pcm_s16le', 'rate': 24000},
                                     'voice': VOICE, 'loudness': 50}},
                'tools': [],
            })
            while time.monotonic() < deadline:
                event = json.loads(socket.recv(timeout=max(.1, deadline-time.monotonic())))
                kind = event.get('type')
                if kind == 'session.created':
                    send('input_audio_mute.commit')
                    send('speech_text_buffer.commit', text=PROMPT)
                elif kind == 'response.output_audio.delta':
                    chunk = base64.b64decode(event.get('delta', ''), validate=True)
                    total += len(chunk)
                    if total > 192000 or len(chunk) % 2:
                        raise PromptError('invalid_prompt_audio')
                    chunks.append(chunk)
                elif kind == 'response.output_audio.done':
                    send('session.close')
                    if total < 9600:
                        raise PromptError('prompt_audio_too_short')
                    return b''.join(chunks)
                elif kind in ('error', 'session.closed'):
                    raise PromptError('prompt_service_ended')
    except PromptError:
        raise
    except Exception:
        raise PromptError('prompt_generation_failed') from None
    raise PromptError('prompt_timeout')

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=Path('.runtime/vivi-wake.pcm'))
    args = parser.parse_args()
    pcm = synthesize(os.environ.get('DOUBAO_API_KEY', ''))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    temporary = args.output.with_name(args.output.name + '.tmp')
    fd = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, 'wb') as stream:
        stream.write(pcm)
    temporary.replace(args.output)
    print(f'Vivi 2.0 greeting generated: {len(pcm)} bytes')

if __name__ == '__main__':
    try:
        main()
    except PromptError as error:
        print(str(error))
        raise SystemExit(1)
