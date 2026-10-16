"""Loop boundary regression: MP3 audio ends before video/container duration."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

WORKER = Path(os.environ.get('GRAPH_WORKER', Path(__file__).resolve().parents[2] / 'build' / 'graph_worker')).resolve()


@unittest.skipUnless(shutil.which('ffmpeg') and shutil.which('ffprobe'), 'Requires ffmpeg/ffprobe')
class GapFillingTests(unittest.TestCase):
    def test_loop_with_shorter_mp3_audio(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source, output = root / 'source.mp4', root / 'output.mp4'
            subprocess.run(['ffmpeg', '-v', 'error', '-f', 'lavfi', '-i',
                            'testsrc2=size=64x48:rate=30:duration=0.5', '-f', 'lavfi', '-i',
                            'sine=sample_rate=48000:duration=0.3', '-c:v', 'mpeg4', '-c:a', 'libmp3lame',
                            str(source)], check=True, timeout=10)
            def node(kind, name, params, connections):
                return dict(type=kind, name=name, configuration=[params], connections=connections)
            graph = [
                node('InputNode', 'input', dict(filename=str(source), loop='true'), ['demux']),
                node('DemuxNode', 'demux', dict(ports='2'), ['vd', 'ad']),
                node('VideoDecoderNode', 'vd', {}, ['vf']),
                node('VideoGapFillerNode', 'vf', {}, ['ve']),
                node('VideoEncoderNode', 've', dict(codec='libx264', preset='ultrafast', tune='zerolatency'),
                     [dict(node='mux', input_port=0)]),
                node('AudioDecoderNode', 'ad', {}, ['af']),
                node('AudioGapFillerNode', 'af', {}, ['ar']),
                node('AudioResamplerNode', 'ar', dict(sample_rate='48000'), ['ae']),
                node('AudioEncoderNode', 'ae', {}, [dict(node='mux', input_port=1)]),
                node('MuxerNode', 'mux', dict(ports='2'), ['out']),
                node('OutputNode', 'out', dict(filename=str(output)), []),
            ]
            config = root / 'config.json'
            config.write_text(json.dumps(dict(graph=graph, queue_capacity=256)))
            with (root / 'worker.log').open('w') as log:
                worker = subprocess.Popen([str(WORKER), str(config)], stdout=log, stderr=log)
                try:
                    try:
                        worker.wait(timeout=1.5)
                        self.fail('Looped graph exited before cancellation')
                    except subprocess.TimeoutExpired:
                        worker.terminate()
                        worker.wait(timeout=10)
                finally:
                    if worker.poll() is None:
                        worker.kill()
                        worker.wait()
            stderr = (root / 'worker.log').read_text()
            self.assertEqual(worker.returncode, 0, stderr)
            self.assertNotIn('code=-', stderr)
            self.assertGreater(stderr.count('read progress=0%'), 3, stderr)
            streams = json.loads(subprocess.check_output([
                'ffprobe', '-v', 'error', '-show_streams', '-of', 'json', str(output)], timeout=5))['streams']
            self.assertEqual([s['codec_name'] for s in streams], ['h264', 'aac'])
            self.assertGreater(float(streams[0]['duration']), 1)
            self.assertGreater(float(streams[1]['duration']), 1)
