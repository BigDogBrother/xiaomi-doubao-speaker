import base64
import importlib.util
import json
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location('prompt', Path(__file__).resolve().parents[2]/'tools/make_wake_prompt.py')
prompt = importlib.util.module_from_spec(spec)
spec.loader.exec_module(prompt)

class Socket:
    def __init__(self, events): self.events=iter(events); self.sent=[]
    def __enter__(self): return self
    def __exit__(self,*_): pass
    def send(self,data): self.sent.append(json.loads(data))
    def recv(self,**_): return json.dumps(next(self.events))

class PromptTests(unittest.TestCase):
    def test_exact_voice_and_fixed_text(self):
        pcm=bytes(9600)
        socket=Socket([{'type':'session.created'}, {'type':'response.output_audio.delta','delta':base64.b64encode(pcm).decode()}, {'type':'response.output_audio.done'}])
        options={}
        def connector(url,**kwargs): options.update(url=url,**kwargs);return socket
        self.assertEqual(prompt.synthesize('unit-test-key',connector),pcm)
        self.assertEqual(options['url'],prompt.ENDPOINT)
        self.assertEqual(socket.sent[0]['session']['audio']['output']['voice'],prompt.VOICE)
        say=next(x for x in socket.sent if x['type']=='speech_text_buffer.commit')
        self.assertEqual(say['text'],'我在，请说。')
        self.assertEqual(socket.sent[-1]['type'],'session.close')
    def test_raw_service_error_is_not_exposed(self):
        socket=Socket([{'type':'error','message':'private-server-detail'}])
        with self.assertRaisesRegex(prompt.PromptError,'^prompt_service_ended$'):
            prompt.synthesize('unit-test-key',lambda *_,**__:socket)
    def test_invalid_pcm_is_rejected(self):
        socket=Socket([{'type':'response.output_audio.delta','delta':base64.b64encode(b'x').decode()}])
        with self.assertRaisesRegex(prompt.PromptError,'invalid_prompt_audio'):
            prompt.synthesize('unit-test-key',lambda *_,**__:socket)

if __name__=='__main__': unittest.main()
