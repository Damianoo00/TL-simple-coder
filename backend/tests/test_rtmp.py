import json
import os
import shutil
import signal
import socket
import subprocess
import tempfile
import time
import unittest
from pathlib import Path


WORKER = Path(os.environ.get('GRAPH_WORKER', './build/graph_worker')).resolve()


@unittest.skipUnless(shutil.which('ffmpeg') and shutil.which('ffprobe'), 'RTMP tests require ffmpeg and ffprobe')
class RTMPTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='async-rtmp-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.source = self.root / 'source.mp4'
        subprocess.run([
            'ffmpeg', '-v', 'error', '-f', 'lavfi', '-i', 'testsrc2=size=64x48:rate=12',
            '-f', 'lavfi', '-i', 'sine=frequency=440:sample_rate=48000',
            '-t', '1.5', '-c:v', 'libx264', '-preset', 'ultrafast', '-pix_fmt', 'yuv420p',
            '-c:a', 'aac', '-y', str(self.source),
        ], check=True, timeout=15)

    def config(self, url):
        path = self.root / 'graph.json'
        path.write_text(json.dumps({'graph': [
            {'name': 'input', 'type': 'InputNode', 'configuration': [{'filename': str(self.source)}], 'connections': ['rtmp']},
            {'name': 'rtmp', 'type': 'RTMPOutputNode', 'configuration': [{'url': url}], 'connections': []},
        ]}))
        return path

    @staticmethod
    def port():
        with socket.socket() as sock:
            sock.bind(('127.0.0.1', 0))
            return sock.getsockname()[1]

    def test_publish_to_local_ffmpeg_listener(self):
        port = self.port()
        url = f'rtmp://127.0.0.1:{port}/live/test'
        received = self.root / 'received.flv'
        listener = subprocess.Popen([
            'ffmpeg', '-v', 'error', '-listen', '1', '-i', url,
            '-c', 'copy', '-y', str(received),
        ], stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
        try:
            time.sleep(0.4)
            result = subprocess.run([str(WORKER), str(self.config(url))], capture_output=True, text=True, timeout=15)
            self.assertEqual(result.returncode, 0, result.stderr)
            _, errors = listener.communicate(timeout=8)
            self.assertEqual(listener.returncode, 0, errors)
            probe = subprocess.run([
                'ffprobe', '-v', 'error', '-show_entries', 'stream=codec_name',
                '-of', 'json', str(received),
            ], capture_output=True, text=True, check=True, timeout=5)
            codecs = {stream['codec_name'] for stream in json.loads(probe.stdout)['streams']}
            self.assertEqual(codecs, {'h264', 'aac'})
        finally:
            if listener.poll() is None:
                listener.terminate()
                listener.communicate(timeout=5)

    def test_connection_failure_is_bounded(self):
        port = self.port()
        url = f'rtmp://127.0.0.1:{port}/live/private-key'
        result = subprocess.run([str(WORKER), str(self.config(url))], capture_output=True, text=True, timeout=10)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('cannot connect', result.stderr)
        self.assertNotIn('private-key', result.stderr)

    def test_stop_interrupts_stalled_handshake(self):
        with socket.socket() as listener:
            listener.bind(('127.0.0.1', 0))
            listener.listen(1)
            listener.settimeout(5)
            url = f'rtmp://127.0.0.1:{listener.getsockname()[1]}/live/test'
            worker = subprocess.Popen([str(WORKER), str(self.config(url))],
                                      stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            try:
                connection, _ = listener.accept()
                with connection:
                    worker.send_signal(signal.SIGTERM)
                    _, errors = worker.communicate(timeout=4)
                self.assertEqual(worker.returncode, 0, errors)
                self.assertNotIn('code=-11', errors)
            finally:
                if worker.poll() is None:
                    worker.kill()
                    worker.communicate(timeout=5)


if __name__ == '__main__':
    unittest.main()
