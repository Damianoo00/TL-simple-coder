import copy
import json
import os
import select
import signal
from pathlib import Path
import subprocess
import tempfile
import unittest


from media_fixture import HAS_MEDIA_TOOLS, create_input, output_ready


WORKER = Path(os.environ.get("GRAPH_WORKER", Path(__file__).resolve().parents[2] / "build" / "graph_worker")).resolve()


@unittest.skipUnless(HAS_MEDIA_TOOLS, 'Tests require ffmpeg and ffprobe')
class JsonGraphTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        create_input(self.root / "input.mp4")
        self.config = {"graph": [
            {"type": "InputNode", "name": "input",
             "configuration": [{"filename": "input.mp4"}], "connections": ["output"]},
            {"type": "OutputNode", "name": "output",
             "configuration": [{"filename": "output.mp4"}], "connections": []},
        ]}

    def write_config(self, configuration, filename="config.json"):
        path = self.root / filename
        path.write_text(json.dumps(configuration))
        return path

    def run_pipeline(self, arguments):
        process = subprocess.Popen([str(WORKER), *arguments], cwd=self.root,
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        try:
            stdout, stderr = process.communicate(timeout=10)
            output = self.root / "output.mp4"
            self.assertEqual(process.returncode, 0, stderr)
            self.assertTrue(output.exists(), "Worker did not create the output file")
            self.assertTrue(output_ready(output), "Worker did not produce a complete MP4")
            self.assertIn("Graph started", stdout)
            self.assertIn("Graph stopped", stdout)
        finally:
            if process.poll() is None:
                process.kill()
                process.communicate()

    def test_default_config_and_forward_references(self):
        self.write_config(self.config)
        self.run_pipeline([])

    def test_explicit_config_path(self):
        path = self.write_config(self.config, "custom.json")
        self.run_pipeline([str(path)])

    def assert_rejected(self, path):
        output = self.root / "output.mp4"
        output.write_text("keep existing output\n")
        result = subprocess.run([str(WORKER), str(path)], cwd=self.root,
                                capture_output=True, text=True, timeout=5)
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertIn("Configuration", result.stderr)
        self.assertNotIn("Graph started", result.stdout)
        self.assertEqual(output.read_text(), "keep existing output\n")

    def test_invalid_configurations_do_not_truncate_output(self):
        mutations = [
            lambda graph: graph[0].update(type="UnknownNode"),
            lambda graph: graph[0].update(type="Node"),
            lambda graph: graph[1].update(name="input"),
            lambda graph: graph[1].update(connections=["missing"]),
            lambda graph: graph[0].update(connections=["input", "output"]),
            lambda graph: graph[1].update(configuration=[]),
            lambda graph: graph[1].update(configuration=[{"filename": 12}]),
            lambda graph: graph[1].update(configuration={"filename": "output.mp4"}),
            lambda graph: graph[1].update(configuration=[{"filename": "output.mp4"}, {"filename": "other.txt"}]),
            lambda graph: graph[1].pop("connections"),
            lambda graph: graph[1].update(connections=[12]),
            lambda graph: graph[1].update(connections=["input"]),
            lambda graph: graph[1].update(name=""),
        ]
        for mutate in mutations:
            configuration = copy.deepcopy(self.config)
            mutate(configuration["graph"])
            with self.subTest(configuration=configuration):
                self.assert_rejected(self.write_config(configuration))
        for configuration in ({"graph": []}, {"graph": {}}, {}, []):
            with self.subTest(configuration=configuration):
                self.assert_rejected(self.write_config(configuration))

    def test_invalid_port_mappings_do_not_truncate_output(self):
        config = {"graph": [
            {"type": "InputNode", "name": "input", "configuration": [{"filename": "input.mp4"}], "connections": ["demux"]},
            {"type": "DemuxNode", "name": "demux", "configuration": [{"ports": "2"}],
             "connections": [{"node": "mux", "output_port": 0, "input_port": 0},
                             {"node": "mux", "output_port": 1, "input_port": 1}]},
            {"type": "MuxerNode", "name": "mux", "configuration": [{"ports": "2"}], "connections": ["output"]},
            {"type": "OutputNode", "name": "output", "configuration": [{"filename": "output.mp4"}], "connections": []},
        ]}
        mutations = [
            lambda graph: graph[1]["connections"][0].update(output_port=-1),
            lambda graph: graph[1]["connections"][0].update(output_port=2),
            lambda graph: graph[1]["connections"][0].update(input_port=2),
            lambda graph: graph[1]["connections"][0].update(input_port="0"),
            lambda graph: graph[1]["connections"][0].update(input_port=0.5),
            lambda graph: graph[1]["connections"][0].pop("node"),
            lambda graph: graph[1]["connections"][1].update(output_port=0),
            lambda graph: graph[1]["connections"][1].update(input_port=0),
            lambda graph: graph[1].update(connections=graph[1]["connections"][:1]),
            lambda graph: graph[1].update(configuration=[{"ports": "0"}]),
            lambda graph: graph[2].update(configuration=[{"ports": "invalid"}]),
        ]
        for mutate in mutations:
            configuration = copy.deepcopy(config)
            mutate(configuration["graph"])
            with self.subTest(configuration=configuration):
                self.assert_rejected(self.write_config(configuration))

    def test_incompatible_codec_connections_do_not_truncate_output(self):
        for decoder, encoder in (("VideoDecoderNode", "AudioEncoderNode"),
                                 ("AudioDecoderNode", "VideoEncoderNode"),
                                 ("VideoDecoderNode", "AudioResamplerNode"),
                                 ("AudioDecoderNode", "VideoScalerNode")):
            config = {"graph": [
                {"type": "InputNode", "name": "input", "configuration": [{"filename": "input.mp4"}], "connections": ["decoder"]},
                {"type": decoder, "name": "decoder", "configuration": [], "connections": ["encoder"]},
                {"type": encoder, "name": "encoder", "configuration": [], "connections": ["output"]},
                {"type": "OutputNode", "name": "output", "configuration": [{"filename": "output.mp4"}], "connections": []},
            ]}
            with self.subTest(decoder=decoder, encoder=encoder):
                self.assert_rejected(self.write_config(config))
        config = copy.deepcopy(self.config)
        config["graph"].insert(1, {"type": "VideoEncoderNode", "name": "encoder", "configuration": [], "connections": ["output"]})
        config["graph"][0]["connections"] = ["encoder"]
        self.assert_rejected(self.write_config(config))

    def test_invalid_json_and_missing_file(self):
        path = self.root / "broken.json"
        path.write_text('{"graph": [],}')
        self.assert_rejected(path)
        self.assert_rejected(self.root / "missing.json")

    def test_missing_input_file(self):
        self.config["graph"][0]["configuration"] = [{"filename": "missing.mp4"}]
        self.assert_rejected(self.write_config(self.config))

    def test_runtime_failure_exits_nonzero(self):
        self.config["graph"] = [self.config["graph"][0]]
        self.config["graph"][0]["connections"] = []
        result = subprocess.run([str(WORKER), str(self.write_config(self.config))],
                                cwd=self.root, capture_output=True, text=True, timeout=5)
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIn("Graph failed: node=0 code=-3", result.stderr)

    def test_decoder_failure_after_start_exits_nonzero(self):
        self.config["graph"] = [
            {"type": "InputNode", "name": "input", "configuration": [{"filename": "input.mp4"}],
             "connections": ["decoder"]},
            {"type": "AudioDecoderNode", "name": "decoder", "configuration": [],
             "connections": ["encoder"]},
            {"type": "AudioEncoderNode", "name": "encoder", "configuration": [],
             "connections": ["output"]},
            {"type": "OutputNode", "name": "output", "configuration": [{"filename": "output.mp4"}],
             "connections": []},
        ]
        result = subprocess.run([str(WORKER), str(self.write_config(self.config))],
                                cwd=self.root, capture_output=True, text=True, timeout=5)
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIn("Graph started", result.stdout)
        self.assertIn("Graph failed: node=1 code=-4", result.stderr)

    def test_signals_cancel_running_graph(self):
        config = {"graph": [{"type": "OutputNode", "name": "idle",
                             "configuration": [{"filename": "idle.mp4"}], "connections": []}]}
        path = self.write_config(config)
        for sig in (signal.SIGINT, signal.SIGTERM):
            with self.subTest(signal=sig):
                process = subprocess.Popen([str(WORKER), str(path)], cwd=self.root,
                                           stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
                try:
                    ready, _, _ = select.select([process.stdout], [], [], 5)
                    self.assertTrue(ready, "Worker did not announce startup")
                    self.assertIn("Graph started", process.stdout.readline())
                    self.assertIsNone(process.poll())
                    process.send_signal(sig)
                    stdout, stderr = process.communicate(timeout=5)
                    self.assertEqual(process.returncode, 0, stderr)
                    self.assertIn("Graph stopped", stdout)
                finally:
                    if process.poll() is None:
                        process.kill()
                        process.communicate()


if __name__ == "__main__":
    unittest.main()
