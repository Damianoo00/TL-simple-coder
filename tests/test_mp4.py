import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time
import unittest

WORKER = Path(os.environ.get('GRAPH_WORKER', Path(__file__).resolve().parents[1] / 'build' / 'graph_worker')).resolve()


@unittest.skipUnless(shutil.which('ffmpeg') and shutil.which('ffprobe'), 'MP4 tests require ffmpeg and ffprobe')
class Mp4Tests(unittest.TestCase):
    def probe(self, path):
        result = subprocess.run(['ffprobe', '-v', 'error', '-show_streams',
                                 '-show_packets', '-show_data_hash', 'sha256',
                                 '-of', 'json', str(path)], capture_output=True, text=True)
        if result.returncode:
            return None
        return json.loads(result.stdout)

    def test_video_and_audio_packets_survive_pipeline(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source, output = root / 'input.mp4', root / 'output.MP4'
            subprocess.run(['ffmpeg', '-v', 'error', '-f', 'lavfi', '-i',
                            'testsrc2=size=64x48:rate=12', '-f', 'lavfi', '-i',
                            'sine=frequency=440:sample_rate=48000', '-t', '1',
                            '-c:v', 'mpeg4', '-bf', '2', '-c:a', 'aac', str(source)], check=True)
            config = {'graph': [
                {'type': 'InputNode', 'name': 'in', 'configuration': [{'filename': str(source)}], 'connections': ['out']},
                {'type': 'OutputNode', 'name': 'out', 'configuration': [{'filename': str(output)}], 'connections': []},
            ]}
            path = root / 'config.json'
            path.write_text(json.dumps(config))
            worker = subprocess.Popen([str(WORKER), str(path)], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            try:
                expected = self.probe(source)
                protocol_output = root / 'protocol.mp4'
                protocol_test = Path(os.environ.get('MEDIA_PROTOCOL_TEST', WORKER.parent / 'test_media_protocol'))
                subprocess.run([str(protocol_test), str(source), str(protocol_output)],
                               capture_output=True, text=True, check=True, timeout=10)
                protocol_result = self.probe(protocol_output)
                self.assertIsNotNone(protocol_result)
                self.assertEqual([p['data_hash'] for p in expected['packets']],
                                 [p['data_hash'] for p in protocol_result['packets']])
                deadline = time.monotonic() + 10
                actual = None
                while time.monotonic() < deadline:
                    actual = self.probe(output)
                    if actual and len(actual.get('packets', [])) == len(expected['packets']):
                        break
                    if worker.poll() is not None:
                        self.fail(worker.communicate()[1])
                    time.sleep(0.05)
                self.assertIsNotNone(actual, 'MP4 must be finalized at EOF before worker termination')
                for key in ('codec_name', 'codec_type', 'width', 'height', 'sample_rate', 'channels'):
                    self.assertEqual([s.get(key) for s in expected['streams']], [s.get(key) for s in actual['streams']])
                fields = ('stream_index', 'pts_time', 'dts_time', 'duration_time', 'data_hash')
                self.assertEqual([tuple(p.get(k) for k in fields) for p in expected['packets']],
                                 [tuple(p.get(k) for k in fields) for p in actual['packets']])
                subprocess.run(['ffmpeg', '-v', 'error', '-xerror', '-i', str(output), '-f', 'null', '-'], check=True)
                worker.terminate()
                _, stderr = worker.communicate(timeout=5)
                self.assertEqual(worker.returncode, 0, stderr)
                self.assertNotIn('Node:', stderr)
            finally:
                if worker.poll() is None:
                    worker.kill()
                    worker.communicate()

    def test_invalid_mp4_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / 'broken.mp4'
            source.write_text('not an MP4')
            path = root / 'config.json'
            path.write_text(json.dumps({'graph': [
                {'type': 'InputNode', 'name': 'in', 'configuration': [{'filename': str(source)}], 'connections': []}]}))
            result = subprocess.run([str(WORKER), str(path)], capture_output=True, text=True, timeout=5)
            self.assertEqual(result.returncode, 1)
            self.assertIn('Cannot open input MP4', result.stderr)
