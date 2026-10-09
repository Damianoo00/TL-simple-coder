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

ROOT = Path(__file__).resolve().parents[1]


class SupervisorApiTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        (self.root / 'input.txt').write_text('1\n-2\n8\n')
        with socket.socket() as probe:
            probe.bind(('127.0.0.1', 0))
            self.port = probe.getsockname()[1]
        self.worker = self.root / 'graph_worker'
        self.worker.symlink_to(ROOT / 'graph_worker')
        self.log = open(self.root / 'supervisor.log', 'w+')
        self.addCleanup(self.log.close)
        self.process = subprocess.Popen(
            [str(ROOT / 'supervisor'), str(self.worker), str(self.port)],
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

    def configuration(self, output='output.txt', input_file='input.txt'):
        return {'graph': [
            {'name': 'input', 'type': 'InputNode',
             'configuration': [{'filename': input_file}], 'connections': ['output']},
            {'name': 'output', 'type': 'OutputNode',
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
        output = self.root / 'output.txt'
        self.wait_for(lambda: output.exists() and output.read_text() == '1\n-2\n8\n')
        path = '/graphs/' + str(graph['graph_id'])
        self.assertEqual(self.request('GET', path)[1]['status'], 'running')
        self.assertEqual(len(self.request('GET', '/graphs')[1]), 1)
        self.assertEqual(self.request('DELETE', path)[1]['status'], 'stopped')
        self.assertEqual(self.request('DELETE', path)[1]['status'], 'stopped')
        with self.assertRaises(ProcessLookupError):
            os.kill(graph['pid'], 0)
        second = self.start(self.configuration('second.txt'))
        self.shutdown()
        self.assertEqual(self.process.returncode, 0)
        with self.assertRaises(ProcessLookupError):
            os.kill(second['pid'], 0)

    def test_invalid_requests_have_no_side_effects(self):
        output = self.root / 'output.txt'
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
        graph = self.start(self.configuration(input_file='missing.txt'))
        path = '/graphs/' + str(graph['graph_id'])
        self.wait_for(lambda: self.request('GET', path)[1]['status'] == 'failed')
        self.assertEqual(self.request('GET', path)[1]['exit_code'], 1)

    def test_concurrent_starts_use_independent_configurations(self):
        with concurrent.futures.ThreadPoolExecutor(max_workers=4) as executor:
            graphs = list(executor.map(lambda i: self.start(self.configuration(f'output-{i}.txt')), range(4)))
        self.assertEqual(len({g['graph_id'] for g in graphs}), 4)
        self.assertEqual(len({g['pid'] for g in graphs}), 4)
        for i in range(4):
            output = self.root / f'output-{i}.txt'
            self.wait_for(lambda: output.exists() and output.read_text() == '1\n-2\n8\n')

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
        graph = self.start()
        output = self.root / 'output.txt'
        self.wait_for(lambda: output.exists() and output.read_text() == '1\n-2\n8\n')
        os.kill(graph['pid'], signal.SIGSTOP)
        started = time.monotonic()
        status, body, _ = self.request('DELETE', '/graphs/' + str(graph['graph_id']))
        self.assertEqual(status, 200)
        self.assertEqual(body['status'], 'stopped')
        self.assertEqual(body['signal'], signal.SIGKILL)
        self.assertLess(time.monotonic() - started, 4)


if __name__ == '__main__':
    unittest.main()
