import copy
import json
import os
from pathlib import Path
import subprocess
import tempfile
import time
import unittest


from media_fixture import HAS_MEDIA_TOOLS, create_input, output_ready


WORKER = Path(os.environ.get("GRAPH_WORKER", Path(__file__).resolve().parents[1] / "build" / "graph_worker")).resolve()


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
            deadline = time.monotonic() + 5
            output = self.root / "output.mp4"
            while time.monotonic() < deadline:
                if output_ready(output):
                    break
                if process.poll() is not None:
                    self.fail("Worker exited before producing output: " + process.communicate()[1])
                time.sleep(0.02)
            self.assertTrue(output.exists(), "Worker did not create the output file")
            self.assertTrue(output_ready(output), "Worker did not produce a complete MP4")
            process.terminate()
            stdout, stderr = process.communicate(timeout=5)
            self.assertEqual(process.returncode, 0, stderr)
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

    def test_invalid_json_and_missing_file(self):
        path = self.root / "broken.json"
        path.write_text('{"graph": [],}')
        self.assert_rejected(path)
        self.assert_rejected(self.root / "missing.json")

    def test_missing_input_file(self):
        self.config["graph"][0]["configuration"] = [{"filename": "missing.mp4"}]
        self.assert_rejected(self.write_config(self.config))


if __name__ == "__main__":
    unittest.main()
