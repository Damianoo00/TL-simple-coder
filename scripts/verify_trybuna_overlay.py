#!/usr/bin/env python3
"""Exercise the running workspace copy of Trybuna with a real Chromium video graph."""
import json
from pathlib import Path
import subprocess
import time
import urllib.request

root = Path(__file__).resolve().parent.parent
base = "http://127.0.0.1:3000"

def request(path, method="GET", body=None):
    payload = None if body is None else json.dumps(body).encode()
    req = urllib.request.Request(base + path, data=payload, method=method, headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=5) as response:
        return json.load(response)

previous = request("/api/state")
configuration = json.loads((root / "examples/overlay.json").read_text())
configuration["graph"][-1]["configuration"] = [{"filename": "build/overlay-validation.mp4"}]
config = root / "build/overlay-validation.json"
config.write_text(json.dumps(configuration))
log = root / "build/overlay-validation.log"
worker = None
try:
    with log.open("w") as output:
        worker = subprocess.Popen([str(root / "build/graph_worker"), str(config)], cwd=root, stdout=output, stderr=subprocess.STDOUT)
        deadline = time.monotonic() + 35
        while "Graph started" not in log.read_text():
            if worker.poll() is not None or time.monotonic() > deadline:
                raise RuntimeError("Worker did not start; inspect " + str(log))
            time.sleep(0.05)
        time.sleep(1.2)
        request("/api/state", "PUT", {"scoreHome": 7, "scoreAway": 3})
        time.sleep(1.2)
        request("/api/scoreboard/hide", "POST", {})
        time.sleep(1)
        request("/api/scoreboard/show", "POST", {})
        deadline = time.monotonic() + 20
        while "reason=end_of_stream" not in log.read_text():
            if worker.poll() is not None or time.monotonic() > deadline:
                raise RuntimeError("Video did not finish; inspect " + str(log))
            time.sleep(0.05)
        worker.terminate()
        worker.wait(timeout=3)
        if worker.returncode:
            raise RuntimeError("Worker failed; inspect " + str(log))
    result = json.loads(subprocess.check_output(["ffprobe", "-v", "error", "-show_streams", "-of", "json",
                                                str(root / "build/overlay-validation.mp4")]))
    video = next(stream for stream in result["streams"] if stream["codec_type"] == "video")
    if video["r_frame_rate"] != "60/1" or not any(s["codec_type"] == "audio" for s in result["streams"]):
        raise RuntimeError("Expected 60 FPS video with preserved audio")
    for stamp, name in [(0.7, "initial"), (2.1, "score"), (3.3, "hidden"), (4.8, "shown")]:
        subprocess.run(["ffmpeg", "-y", "-v", "error", "-ss", str(stamp), "-i", str(root / "build/overlay-validation.mp4"),
                        "-frames:v", "1", "-update", "1", str(root / f"build/overlay-{name}.png")], check=True)
    print("Verified 60 FPS with audio; controller changed score and hid/showed the overlay.")
    print("Video: build/overlay-validation.mp4; metrics: build/overlay-validation.log; previews: build/overlay-*.png")
finally:
    if worker is not None and worker.poll() is None:
        worker.terminate()
        try:
            worker.wait(timeout=3)
        except subprocess.TimeoutExpired:
            worker.kill()
            worker.wait()
    request("/api/state", "PUT", {key: previous[key] for key in ["scoreHome", "scoreAway", "time", "half"]})
    request("/api/scoreboard/" + ("show" if previous["scoreboardVisible"] else "hide"), "POST", {})
