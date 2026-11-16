"""Opt-in single local inference check. No downloads or saved application settings."""
import argparse
import base64
import json
import os
from pathlib import Path
import secrets
import socket
import subprocess
import time
import urllib.error
import urllib.request

parser = argparse.ArgumentParser()
for name in ("python", "plugin", "model", "diffusion", "vae", "output"):
    parser.add_argument("--" + name, required=True)
parser.add_argument("--images", nargs="*", default=[])
parser.add_argument("--max-vram", type=float, default=6)
args = parser.parse_args()
plugin = Path(args.plugin).resolve()
output = Path(args.output).resolve()
output.parent.mkdir(parents=True, exist_ok=True)
key = secrets.token_hex(24)
with socket.socket() as temporary:
    temporary.bind(("127.0.0.1", 0))
    port = temporary.getsockname()[1]
config = {"engine": "sdcpp", "model_directory": args.model, "diffusion_model": args.diffusion,
          "vae": args.vae, "model_id": "qwen-local-check", "port": port, "key": key,
          "parent_pid": os.getpid(), "retain": True, "turbo": True, "max_vram": args.max_vram}
http = urllib.request.build_opener(urllib.request.ProxyHandler({}))


def request(path, data=None, content_type="application/json", timeout=10):
    return http.open(urllib.request.Request(
        f"http://127.0.0.1:{port}" + path, data=data,
        headers={"Authorization": "Bearer " + key, "Content-Type": content_type}), timeout=timeout)


started = time.monotonic()
with output.with_suffix(".log").open("wb") as log:
    child = subprocess.Popen([args.python, "-I", "-u", str(plugin / "server.py")],
                             stdin=subprocess.PIPE, stdout=log, stderr=log,
                             creationflags=subprocess.CREATE_NO_WINDOW)
    try:
        child.stdin.write(json.dumps(config).encode())
        child.stdin.close()
        for _ in range(150):
            if child.poll() is not None:
                raise RuntimeError("Service exited; see " + str(output.with_suffix(".log")))
            try:
                with request("/health") as response:
                    health = json.load(response)
                break
            except (OSError, urllib.error.URLError):
                time.sleep(.2)
        else:
            raise RuntimeError("Service readiness timed out")
        fields = {"model": config["model_id"], "prompt": (
            "Replace the person in image 1 with the character from image 2. Keep the pose, "
            "composition and background of image 1. Follow the style in image 3."
            if len(args.images) >= 3 else "A cheerful cartoon character waving, flat colors, white background."),
            "stream": True, "partial_images": 3, "size": "512x512", "seed": 42}
        content_type = "application/json"
        path = "/v1/images/generations"
        if args.images:
            boundary = "qwen-test-" + secrets.token_hex(8)
            parts = []
            for name, value in fields.items():
                value = json.dumps(value) if not isinstance(value, str) else value
                parts.append(f'--{boundary}\r\nContent-Disposition: form-data; name="{name}"\r\n\r\n{value}\r\n'.encode())
            for index, filename in enumerate(args.images):
                parts.append(f'--{boundary}\r\nContent-Disposition: form-data; name="image[]"; '
                             f'filename="reference-{index}.png"\r\nContent-Type: image/png\r\n\r\n'.encode()
                             + Path(filename).read_bytes() + b"\r\n")
            data = b"".join(parts) + f"--{boundary}--\r\n".encode()
            path, content_type = "/v1/images/edits", "multipart/form-data; boundary=" + boundary
        else:
            data = json.dumps(fields).encode()
        print("Local model loading / inference started", flush=True)
        completed = False
        with request(path, data, content_type, timeout=1200) as response:
            for line in response:
                if time.monotonic() - started > 1200:
                    raise TimeoutError("Live inference exceeded twenty minutes")
                if not line.startswith(b"data: "):
                    continue
                raw = line[6:].strip()
                if raw == b"[DONE]":
                    break
                event = json.loads(raw)
                if "error" in event:
                    raise RuntimeError(str(event["error"]))
                if event.get("type", "").endswith(".partial_image"):
                    print("Received partial image", event["partial_image_index"], flush=True)
                if event.get("type", "").endswith(".completed"):
                    output.write_bytes(base64.b64decode(event["b64_json"], validate=True))
                    completed = True
        if not completed:
            raise RuntimeError("No completed image")
        with request("/health") as response:
            assert json.load(response)["loaded"]
        with request("/admin/unload", b"{}", timeout=20) as response:
            assert not json.load(response)["loaded"]
        print(json.dumps({"output": str(output), "seconds": round(time.monotonic() - started, 2),
                          "references": len(args.images), "manual_unload": "passed"}), flush=True)
    finally:
        child.terminate()
        try:
            child.wait(timeout=10)
        except subprocess.TimeoutExpired:
            child.kill()
            child.wait(timeout=10)
