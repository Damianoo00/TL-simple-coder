import json
import shutil
import subprocess

HAS_MEDIA_TOOLS = bool(shutil.which('ffmpeg') and shutil.which('ffprobe'))


def create_input(path):
    subprocess.run(['ffmpeg', '-v', 'error', '-f', 'lavfi', '-i',
                    'testsrc2=size=64x48:rate=12', '-frames:v', '3',
                    '-c:v', 'mpeg4', str(path)], check=True)


def output_ready(path):
    if not path.exists():
        return False
    result = subprocess.run(['ffprobe', '-v', 'error', '-count_packets',
                             '-show_entries', 'stream=nb_read_packets', '-of', 'json',
                             str(path)], capture_output=True, text=True)
    if result.returncode:
        return False
    return [s.get('nb_read_packets') for s in json.loads(result.stdout).get('streams', [])] == ['3']
