import concurrent.futures
import http.client
import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import tempfile
import time
import unittest

from media_fixture import HAS_MEDIA_TOOLS, create_input, output_ready


ROOT = Path(__file__).resolve().parents[1]
WORKER = Path(os.environ.get('GRAPH_WORKER', ROOT / 'build' / 'graph_worker')).resolve()
SUPERVISOR = Path(os.environ.get('SUPERVISOR', ROOT / 'build' / 'supervisor')).resolve()


@unittest.skipUnless(HAS_MEDIA_TOOLS, 'Tests require ffmpeg and ffprobe')
class SupervisorApiTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        create_input(self.root / 'input.mp4')
        with socket.socket() as probe:
            probe.bind(('127.0.0.1', 0))
            self.port = probe.getsockname()[1]
        self.worker = self.root / 'graph_worker'
        self.worker.symlink_to(WORKER)
        self.log = open(self.root / 'supervisor.log', 'w+')
        self.addCleanup(self.log.close)
        self.process = subprocess.Popen(
            [str(SUPERVISOR), str(self.worker), str(self.port)],
            cwd=self.root, stdout=self.log, stderr=self.log)
        self.addCleanup(self.shutdown)
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            try:
                if self.request('GET', '/graphs')[0] == 200:
                    return
            except OSError:
                pass
            if self.process.poll() is not None:
                self.log.seek(0)
                self.fail('Supervisor exited: ' + self.log.read())
            time.sleep(0.02)
        self.fail('API did not start')

    def shutdown(self):
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()

    def request(self, method, path, body=None, content_type='application/json'):
        connection = http.client.HTTPConnection('127.0.0.1', self.port, timeout=5)
        try:
            connection.request(method, path, body=body, headers={'Content-Type': content_type})
            response = connection.getresponse()
            return response.status, json.loads(response.read()), dict(response.getheaders())
        finally:
            connection.close()

    def configuration(self, output='output.mp4', input_file='input.mp4'):
        return {'graph': [
            {'name': 'input', 'type': 'InputNode',
             'configuration': [{'filename': input_file}], 'connections': ['output']},
            {'name': 'output', 'type': 'OutputNode',
             'configuration': [{'filename': output}], 'connections': []},
        ]}

    def idle_configuration(self, output='idle.mp4'):
        return {'graph': [
            {'name': 'idle', 'type': 'OutputNode',
             'configuration': [{'filename': output}], 'connections': []},
        ]}

    def start(self, config=None):
        status, body, headers = self.request('POST', '/graphs', json.dumps(config or self.configuration()))
        self.assertEqual(status, 202, body)
        self.assertEqual(body['status'], 'starting')
        self.assertEqual(headers['Location'], '/graphs/' + str(body['graph_id']))
        return body

    def wait_for(self, predicate):
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            if predicate():
                return
            time.sleep(0.02)
        self.fail('Timed out waiting for worker')

    def test_pipeline_status_stop_and_shutdown(self):
        graph = self.start()
        output = self.root / 'output.mp4'
        self.wait_for(lambda: output_ready(output))
        path = '/graphs/' + str(graph['graph_id'])
        self.wait_for(lambda: self.request('GET', path)[1]['status'] == 'stopped')
        self.assertEqual(self.request('GET', path)[1]['exit_code'], 0)
        self.assertEqual(len(self.request('GET', '/graphs')[1]), 1)
        self.assertEqual(self.request('DELETE', path)[1]['status'], 'stopped')
        self.assertEqual(self.request('DELETE', path)[1]['status'], 'stopped')
        with self.assertRaises(ProcessLookupError):
            os.kill(graph['pid'], 0)
        second = self.start(self.idle_configuration('second.mp4'))
        self.wait_for(lambda: (self.root / 'second.mp4').exists())
        self.shutdown()
        self.assertEqual(self.process.returncode, 0)
        with self.assertRaises(ProcessLookupError):
            os.kill(second['pid'], 0)

    def test_invalid_requests_have_no_side_effects(self):
        output = self.root / 'output.mp4'
        output.write_text('keep')
        for body in ('{', '{}', '[]', json.dumps({'graph': []})):
            self.assertEqual(self.request('POST', '/graphs', body)[0], 400)
        config = self.configuration()
        config['graph'][0]['connections'] = ['missing']
        self.assertEqual(self.request('POST', '/graphs', json.dumps(config))[0], 400)
        self.assertEqual(self.request('POST', '/graphs', '{}', 'text/plain')[0], 415)
        self.assertEqual(self.request('POST', '/graphs', ' ' * (1024 * 1024 + 1))[0], 413)
        self.assertEqual(self.request('GET', '/graphs')[1], [])
        self.assertEqual(output.read_text(), 'keep')
        self.assertEqual(self.request('GET', '/graphs/999')[0], 404)
        self.assertEqual(self.request('DELETE', '/graphs/999')[0], 404)

    def test_worker_failure_is_reported(self):
        graph = self.start(self.configuration(input_file='missing.mp4'))
        path = '/graphs/' + str(graph['graph_id'])
        self.wait_for(lambda: self.request('GET', path)[1]['status'] == 'failed')
        self.assertEqual(self.request('GET', path)[1]['exit_code'], 1)
        status, logs, _ = self.request('GET', path + '/logs')
        self.assertEqual(status, 200)
        self.assertTrue(logs['text'])

    def test_runtime_decoder_failure_is_reported(self):
        config = {'graph': [
            {'name': 'input', 'type': 'InputNode', 'configuration': [{'filename': 'input.mp4'}],
             'connections': ['decoder']},
            {'name': 'decoder', 'type': 'AudioDecoderNode', 'configuration': [],
             'connections': ['encoder']},
            {'name': 'encoder', 'type': 'AudioEncoderNode', 'configuration': [],
             'connections': ['output']},
            {'name': 'output', 'type': 'OutputNode', 'configuration': [{'filename': 'output.mp4'}],
             'connections': []},
        ]}
        graph = self.start(config)
        path = '/graphs/' + str(graph['graph_id'])
        self.wait_for(lambda: self.request('GET', path)[1]['status'] == 'failed')
        self.assertEqual(self.request('GET', path)[1]['exit_code'], 1)

    def test_concurrent_starts_use_independent_configurations(self):
        with concurrent.futures.ThreadPoolExecutor(max_workers=4) as executor:
            graphs = list(executor.map(lambda i: self.start(self.configuration(f'output-{i}.mp4')), range(4)))
        self.assertEqual(len({g['graph_id'] for g in graphs}), 4)
        self.assertEqual(len({g['pid'] for g in graphs}), 4)
        for i in range(4):
            output = self.root / f'output-{i}.mp4'
            self.wait_for(lambda: output_ready(output))
            path = '/graphs/' + str(graphs[i]['graph_id'])
            self.wait_for(lambda: self.request('GET', path)[1]['status'] == 'stopped')
            self.assertEqual(self.request('GET', path)[1]['exit_code'], 0)

    def test_spawn_failure_does_not_register_a_graph(self):
        self.worker.unlink()
        status, body, _ = self.request('POST', '/graphs', json.dumps(self.configuration()))
        self.assertEqual(status, 500)
        self.assertIn('error', body)
        self.assertEqual(self.request('GET', '/graphs')[1], [])

    def test_content_type_with_parameters(self):
        status, body, _ = self.request('POST', '/graphs', json.dumps(self.configuration()),
                                       'Application/JSON; charset=utf-8')
        self.assertEqual(status, 202, body)

    def test_forced_stop_is_bounded(self):
        graph = self.start(self.idle_configuration())
        self.wait_for(lambda: (self.root / 'idle.mp4').exists())
        self.assertEqual(self.request('GET', '/graphs/' + str(graph['graph_id']))[1]['status'], 'running')
        os.kill(graph['pid'], signal.SIGSTOP)
        started = time.monotonic()
        status, body, _ = self.request('DELETE', '/graphs/' + str(graph['graph_id']))
        self.assertEqual(status, 200)
        self.assertEqual(body['status'], 'stopped')
        self.assertEqual(body['signal'], signal.SIGKILL)
        self.assertLess(time.monotonic() - started, 4)

    def test_library_persists_updates_and_rejects_invalid_documents(self):
        document = {'name': 'My graph', 'configuration': self.configuration(),
                    'editor': {'nodes': [], 'edges': []}}
        self.assertEqual(self.request('POST', '/library', json.dumps({'name': 'bad'}))[0], 400)
        self.assertEqual(self.request('POST', '/library', json.dumps(document), 'text/plain')[0], 415)
        status, created, headers = self.request('POST', '/library', json.dumps(document))
        self.assertEqual(status, 201)
        graph_id = created['id']
        self.assertEqual(headers['Location'], '/library/' + graph_id)
        self.assertEqual(self.request('GET', '/library')[1], [created])
        document['name'] = 'Updated'
        status, saved, _ = self.request('PUT', '/library/' + graph_id, json.dumps(document))
        self.assertEqual(status, 200)
        self.assertEqual(saved['name'], 'Updated')
        self.assertEqual(self.request('GET', '/library/' + graph_id)[1], saved)
        self.assertEqual(self.request('GET', '/library/invalid-id')[0], 404)
        self.shutdown()
        self.process = subprocess.Popen(
            [str(SUPERVISOR), str(self.worker), str(self.port)],
            cwd=self.root, stdout=self.log, stderr=self.log)
        def library_ready():
            try:
                return self.request('GET', '/library/' + graph_id)[0] == 200
            except OSError:
                return False
        self.wait_for(library_ready)
        self.assertEqual(self.request('GET', '/library/' + graph_id)[1], saved)
        self.assertEqual(self.request('DELETE', '/library/' + graph_id)[0], 200)
        self.assertEqual(self.request('GET', '/library/' + graph_id)[0], 404)

    def test_saved_graph_start_is_unique_and_logs_survive_exit(self):
        document = {'name': 'Idle', 'configuration': self.idle_configuration(),
                    'editor': {'nodes': [], 'edges': []}}
        status, saved, _ = self.request('POST', '/library', json.dumps(document))
        self.assertEqual(status, 201)
        path = '/library/' + saved['id'] + '/start'
        status, graph, _ = self.request('POST', path)
        self.assertEqual(status, 202)
        self.assertEqual(graph['document_id'], saved['id'])
        graph_path = '/graphs/' + str(graph['graph_id'])
        self.wait_for(lambda: (self.root / 'idle.mp4').exists())
        self.assertEqual(self.request('POST', path)[0], 409)
        self.assertEqual(self.request('GET', graph_path)[1]['document_id'], saved['id'])
        self.assertEqual(self.request('GET', '/graphs')[1][0]['document_id'], saved['id'])
        self.assertEqual(self.request('GET', graph_path + '/logs')[0], 200)
        self.assertEqual(self.request('GET', '/graphs/999/logs')[0], 404)
        self.assertEqual(self.request('DELETE', graph_path)[0], 200)
        self.assertEqual(self.request('GET', graph_path + '/logs')[0], 200)
        self.assertEqual(self.request('POST', path)[0], 202)

    def test_logs_since_offset_and_invalid_query(self):
        self.worker.unlink()
        self.worker.write_text('#!/usr/bin/env python3\nimport os, time\nos.write(1, b"hello")\n'
                               'time.sleep(0.5)\nos.write(2, b" world")\n')
        self.worker.chmod(0o755)
        graph = self.start()
        path = '/graphs/' + str(graph['graph_id']) + '/logs'
        self.wait_for(lambda: self.request('GET', path)[1]['text'] == 'hello')
        status, first, _ = self.request('GET', path)
        self.assertEqual(status, 200)
        self.assertEqual(first['offset'], 5)
        self.wait_for(lambda: self.request('GET', path + '?since=5')[1]['text'] == ' world')
        status, second, _ = self.request('GET', path + '?since=' + str(first['offset']))
        self.assertEqual(status, 200)
        self.assertEqual(second, {'text': ' world', 'offset': 11})
        self.assertEqual(self.request('GET', path + '?since=999999')[1], {'text': '', 'offset': 11})
        for invalid in ('', '-1', '1x', '18446744073709551616'):
            self.assertEqual(self.request('GET', path + '?since=' + invalid)[0], 400)

    def test_logs_are_bounded_and_invalid_utf8_is_replaced(self):
        self.worker.unlink()
        self.worker.write_text('#!/usr/bin/env python3\nimport os\nos.write(1, b"x" * 70000 + b"\\xff")\n')
        self.worker.chmod(0o755)
        graph = self.start()
        path = '/graphs/' + str(graph['graph_id'])
        self.wait_for(lambda: self.request('GET', path)[1]['status'] == 'stopped')
        status, logs, _ = self.request('GET', path + '/logs')
        self.assertEqual(status, 200)
        self.assertEqual(logs['offset'], 70001)
        self.assertLessEqual(len(logs['text']), 65536)
        self.assertTrue(logs['text'].endswith('\ufffd'))
        self.assertEqual(self.request('GET', path + '/logs?since=70000')[1]['text'], '\ufffd')


if __name__ == '__main__':
    unittest.main()
