import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time
import unittest

WORKER = Path(os.environ.get('GRAPH_WORKER', Path(__file__).resolve().parents[1] / 'build' / 'graph_worker')).resolve()


@unittest.skipUnless(shutil.which('ffmpeg') and shutil.which('ffprobe'), 'Transcode tests require ffmpeg and ffprobe')
class TranscodeTests(unittest.TestCase):
    def test_video_and_audio(self):
        self.transcode(audio=True)

    def test_video_with_unused_audio_port(self):
        self.transcode(audio=False)

    def transcode(self, audio):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source, output = root / 'input.mp4', root / 'output.mp4'
            command = ['ffmpeg', '-v', 'error', '-f', 'lavfi', '-i', 'testsrc2=size=64x48:rate=12']
            if audio:
                command += ['-f', 'lavfi', '-i', 'sine=frequency=440:sample_rate=48000']
            command += ['-t', '1', '-c:v', 'mpeg4', '-bf', '2', '-c:a', 'aac', str(source)]
            subprocess.run(command, check=True, timeout=10)
            def node(kind, name, configuration, connections):
                return {'type': kind, 'name': name, 'configuration': [configuration], 'connections': connections}
            graph = [
                node('InputNode', 'input', {'filename': str(source)}, ['demux']),
                node('DemuxNode', 'demux', {'ports': '2'}, ['video_decoder', 'audio_decoder']),
                node('VideoDecoderNode', 'video_decoder', {}, ['video_encoder']),
                node('AudioDecoderNode', 'audio_decoder', {}, ['audio_encoder']),
                node('VideoEncoderNode', 'video_encoder', {'max_b_frames': '2'},
                     [{'node': 'mux', 'input_port': 0}]),
                node('AudioEncoderNode', 'audio_encoder', {'bit_rate': '96000'},
                     [{'node': 'mux', 'input_port': 1}]),
                node('MuxerNode', 'mux', {'ports': '2'}, ['output']),
                node('OutputNode', 'output', {'filename': str(output)}, []),
            ]
            config = root / 'config.json'
            config.write_text(json.dumps({'graph': graph}))
            worker = subprocess.Popen([str(WORKER), str(config)], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            try:
                actual = None
                deadline = time.monotonic() + 10
                while time.monotonic() < deadline:
                    result = subprocess.run(['ffprobe', '-v', 'error', '-count_frames', '-show_streams', '-of', 'json', str(output)],
                                            capture_output=True, text=True, timeout=5)
                    if not result.returncode:
                        candidate = json.loads(result.stdout)['streams']
                        if candidate and candidate[0].get('nb_read_frames') == '12':
                            actual = candidate
                            break
                    if worker.poll() is not None:
                        self.fail(worker.communicate()[1])
                    time.sleep(0.03)
                worker.terminate()
                _, stderr = worker.communicate(timeout=5)
                self.assertEqual(worker.returncode, 0, stderr)
                self.assertIsNotNone(actual, stderr)
                self.assertNotIn('code=-', stderr)
                self.assertEqual([s['codec_name'] for s in actual], ['h264', 'aac'] if audio else ['h264'])
                self.assertEqual((actual[0]['width'], actual[0]['height']), (64, 48))
                self.assertAlmostEqual(float(actual[0]['duration']), 1.0, delta=0.02)
                if audio:
                    self.assertEqual((actual[1]['sample_rate'], actual[1]['channels']), ('48000', 1))
                    self.assertAlmostEqual(float(actual[1]['duration']), 1.0, delta=0.05)
                subprocess.run(['ffmpeg', '-v', 'error', '-xerror', '-i', str(output), '-f', 'null', '-'], check=True, timeout=10)
            finally:
                if worker.poll() is None:
                    worker.kill()
                    worker.communicate()
