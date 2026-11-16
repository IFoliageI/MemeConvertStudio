"""Native stable-diffusion.cpp inference, with a small local control bridge."""
import argparse
import base64
import hashlib
import io
import json
import os
from pathlib import Path
import platform
import socket
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.request
import zipfile

ROOT = Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT))
from model_info import components

# Version whose Qwen 2.1 and native job API are used by this adapter.
RELEASE = "master-1106-532d2c1"
API = f"https://api.github.com/repos/leejet/stable-diffusion.cpp/releases/tags/{RELEASE}"
SIGMAS = [1.0, .978453, .95418, .926626, .89508, .845148, .704534, .414568, 0.0]
HTTP = urllib.request.build_opener(urllib.request.ProxyHandler({}))


class CrashJob:
    """Also release the native child if the Python bridge itself crashes."""
    def __init__(self, child):
        self.handle = None
        if os.name != "nt":
            return
        import ctypes as c
        from ctypes import wintypes as w
        class Basic(c.Structure):
            _fields_ = [("process_time", c.c_int64), ("job_time", c.c_int64), ("flags", w.DWORD),
                        ("min_working", c.c_size_t), ("max_working", c.c_size_t),
                        ("active", w.DWORD), ("affinity", c.c_size_t),
                        ("priority", w.DWORD), ("scheduling", w.DWORD)]
        class IO(c.Structure):
            _fields_ = [(name, c.c_uint64) for name in ("read_ops", "write_ops", "other_ops",
                                                       "read_bytes", "write_bytes", "other_bytes")]
        class Extended(c.Structure):
            _fields_ = [("basic", Basic), ("io", IO), ("process_memory", c.c_size_t),
                        ("job_memory", c.c_size_t), ("peak_process", c.c_size_t), ("peak_job", c.c_size_t)]
        self.kernel = c.WinDLL("kernel32", use_last_error=True)
        self.kernel.CreateJobObjectW.argtypes = [c.c_void_p, w.LPCWSTR]
        self.kernel.CreateJobObjectW.restype = w.HANDLE
        self.kernel.SetInformationJobObject.argtypes = [w.HANDLE, c.c_int, c.c_void_p, w.DWORD]
        self.kernel.AssignProcessToJobObject.argtypes = [w.HANDLE, w.HANDLE]
        self.kernel.CloseHandle.argtypes = [w.HANDLE]
        limits = Extended()
        limits.basic.flags = 0x2000
        self.handle = self.kernel.CreateJobObjectW(None, None)
        if not self.handle or not self.kernel.SetInformationJobObject(
                self.handle, 9, c.byref(limits), c.sizeof(limits)) or not self.kernel.AssignProcessToJobObject(
                self.handle, w.HANDLE(int(child._handle))):
            self.close()
            child.kill()
            child.wait()
            raise RuntimeError("Cannot attach native engine to crash cleanup job")

    def close(self):
        if self.handle:
            self.kernel.CloseHandle(self.handle)
            self.handle = None

    def __del__(self):
        self.close()


def executable(config):
    custom = config.get("sd_executable", "").strip()
    if custom:
        path = Path(custom).expanduser().resolve()
        if path.is_dir():
            path /= "sd-server.exe"
        if not path.is_file():
            raise ValueError("sd-server executable does not exist")
        return path
    manifest = ROOT / "runtime" / "installed.json"
    if manifest.is_file():
        path = (ROOT / "runtime" / json.loads(manifest.read_text(encoding="utf-8"))["executable"]).resolve()
        if (ROOT / "runtime").resolve() in path.parents and path.is_file():
            return path
    raise ValueError("Install the native runtime first, or select an existing sd-server.exe in GGUF settings")


def verify(config):
    path = executable(config)
    result = subprocess.run([str(path), "--help"], capture_output=True, text=True,
                            encoding="utf-8", errors="replace", timeout=30)
    if result.returncode != 0 or "--listen-port" not in result.stdout + result.stderr:
        raise RuntimeError("Selected program is not a compatible stable-diffusion.cpp sd-server")
    print(f"Native runtime verified: {path}", flush=True)
    devices = subprocess.run([str(path), "--list-devices"], capture_output=True, text=True,
                             encoding="utf-8", errors="replace", timeout=30)
    print(devices.stdout + devices.stderr, flush=True)
    if devices.returncode != 0:
        raise RuntimeError("Native backend device enumeration failed")
    expected = config.get("backend", "auto")
    if expected in ("vulkan", "cuda") and expected not in (devices.stdout + devices.stderr).lower():
        raise RuntimeError(f"{expected} runtime installed but no corresponding device was detected")


def install(config):
    if config.get("sd_executable"):
        verify(config)
        return
    if os.name != "nt" or platform.machine().lower() not in ("amd64", "x86_64"):
        raise RuntimeError("Automatic native runtime download requires Windows x64. "
                           "Select a compatible custom sd-server build for other architectures.")
    backend = config.get("backend", "auto")
    if backend == "existing":
        verify(config)
        return
    if backend == "auto":
        # Vulkan uses the driver-provided runtime across AMD, Intel and NVIDIA.
        import ctypes
        try:
            ctypes.WinDLL("vulkan-1.dll")
            backend = "vulkan"
        except OSError:
            backend = "cpu"
    suffix = {"vulkan": "vulkan", "cpu": "cpu", "cuda": "cuda12"}.get(backend)
    if not suffix:
        raise RuntimeError("Native engine supports Auto / Vulkan / CPU / CUDA; XPU is a Diffusers backend")
    manifest = ROOT / "runtime" / "installed.json"
    if manifest.is_file():
        installed = json.loads(manifest.read_text(encoding="utf-8"))
        if installed.get("backend") == suffix:
            try:
                verify({**config, "backend": backend})
                print("Reusing the installed native runtime.", flush=True)
                return
            except (ValueError, RuntimeError, OSError, subprocess.SubprocessError):
                print("Installed runtime failed validation; downloading a clean copy.", flush=True)
    with urllib.request.urlopen(urllib.request.Request(API, headers={"User-Agent": "MemeStudio-Qwen"}), timeout=30) as r:
        release = json.load(r)
    asset = next((x for x in release["assets"] if x["name"].endswith(f"-bin-win-{suffix}-x64.zip")
                  and not x["name"].startswith("cudart-")), None)
    if not asset:
        raise RuntimeError("Official runtime archive was not found")
    digest = asset.get("digest", "")
    if not digest.startswith("sha256:"):
        raise RuntimeError("Official release has no SHA256 digest; select a verified custom runtime instead")
    target = ROOT / "runtime" / (RELEASE + "-" + suffix)
    target.mkdir(parents=True, exist_ok=True)
    archive = target / "runtime.zip.part"
    print(f"Downloading official {suffix} inference runtime ({asset['size'] // 1048576} MiB)", flush=True)
    hasher = hashlib.sha256()
    try:
        with urllib.request.urlopen(asset["browser_download_url"], timeout=60) as source, archive.open("wb") as output:
            while data := source.read(1024 * 1024):
                output.write(data)
                hasher.update(data)
        if hasher.hexdigest() != digest.split(":", 1)[1]:
            raise RuntimeError("Runtime archive SHA256 mismatch")
        with zipfile.ZipFile(archive) as bundle:
            for member in bundle.infolist():
                path = (target / member.filename).resolve()
                if target.resolve() not in path.parents:
                    raise RuntimeError("Invalid runtime archive path")
                bundle.extract(member, target)
        matches = list(target.rglob("sd-server.exe"))
        if len(matches) != 1:
            raise RuntimeError("Runtime archive does not contain exactly one sd-server.exe")
        verify({**config, "backend": "cuda" if suffix == "cuda12" else suffix,
                "sd_executable": str(matches[0])})
        manifest = ROOT / "runtime" / "installed.json"
        temporary = manifest.with_suffix(".tmp")
        temporary.write_text(json.dumps({"executable": str(matches[0].relative_to(ROOT / "runtime")),
                                        "backend": suffix, "sha256": hasher.hexdigest()}), encoding="utf-8")
        temporary.replace(manifest)
    finally:
        archive.unlink(missing_ok=True)
    print("Native runtime installed; no GPU driver or system toolkit was changed.", flush=True)


class NativeEngine:
    def __init__(self, config):
        self.config = config
        self.paths, missing = components(config)
        if missing:
            raise ValueError("Missing/ambiguous native model components: " + ", ".join(missing) +
                             ". Text encoder and mmproj alone cannot generate images; open GGUF settings.")
        self.executable = executable(config)
        self.pipeline_class, self.mode = "stable-diffusion.cpp Qwen-Image", "hybrid"
        self.lock, self.process, self.port = threading.Lock(), None, None
        self.job = None
        self.preview = None

    def status(self):
        return {"model": self.config["model_id"], "pipeline": self.pipeline_class,
                "loaded": self.process is not None and self.process.poll() is None,
                "retain": self.config.get("retain", True), "device": "native runtime"}

    def request(self, path, data=None, timeout=60):
        raw = None if data is None else json.dumps(data).encode()
        request = urllib.request.Request(f"http://127.0.0.1:{self.port}" + path, data=raw,
                                         headers={"Content-Type": "application/json"})
        try:
            with HTTP.open(request, timeout=timeout) as response:
                return json.load(response)
        except urllib.error.HTTPError as error:
            raise RuntimeError("Native engine HTTP " + str(error.code) + ": " +
                               error.read(4000).decode("utf-8", errors="replace"))

    def unload(self):
        child, self.process = self.process, None
        job, self.job = self.job, None
        if job:
            job.close()
        self.preview = None
        if child is not None and child.poll() is None:
            child.terminate()
            try:
                child.wait(timeout=5)
            except subprocess.TimeoutExpired:
                child.kill()
                child.wait(timeout=5)

    def load(self):
        if self.process is not None and self.process.poll() is None:
            return
        self.unload()
        with socket.socket() as temporary:
            temporary.bind(("127.0.0.1", 0))
            self.port = temporary.getsockname()[1]
        args = [str(self.executable), "--listen-ip", "127.0.0.1", "--listen-port", str(self.port),
                "--diffusion-model", str(self.paths["diffusion_model"]),
                "--vae", str(self.paths["vae"]), "--llm", str(self.paths["llm"])]
        if "llm_vision" in self.paths:
            args += ["--llm_vision", str(self.paths["llm_vision"])]
        if self.config.get("offload"):
            args += ["--offload-to-cpu"]
        budget = float(self.config.get("max_vram", 0))
        if not 0 <= budget <= 256:
            raise ValueError("VRAM budget must be 0..256 GiB")
        if budget:
            args += ["--max-vram", str(budget)]
        device = self.config.get("device", "auto")
        if device in ("cpu", "cuda", "vulkan"):
            args += ["--backend", device if device == "cpu" else device + "0"]
        elif device != "auto":
            raise ValueError("Native engine device must be Auto / CPU / CUDA / Vulkan")
        self.process = subprocess.Popen(args, stdin=subprocess.DEVNULL,
                                        creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
        self.job = CrashJob(self.process)
        started = time.monotonic()
        try:
            while time.monotonic() - started < 3600:
                if self.process.poll() is not None:
                    raise RuntimeError("Native engine exited while loading; check runtime log")
                try:
                    status = self.request("/sdcpp/v1/models/status", timeout=2)
                    if status.get("state") == "ready":
                        return
                    if status.get("state") in ("error", "empty"):
                        raise RuntimeError("Native model failed to load: " + str(status.get("error")))
                except (urllib.error.URLError, TimeoutError, ConnectionError):
                    pass
                time.sleep(.3)
            raise RuntimeError("Native model loading exceeded one hour")
        except BaseException:
            self.unload()
            raise

    def generate(self, fields, uploads):
        from PIL import Image
        if uploads and "llm_vision" not in self.paths:
            raise ValueError("Image editing with a GGUF text encoder requires the matching mmproj")
        # Decode before loading large weights, and retain each reference's order.
        for data in uploads:
            with Image.open(io.BytesIO(data)) as image:
                if image.width * image.height > 32_000_000:
                    raise ValueError("Reference image exceeds 32 megapixels")
                image.verify()
        self.preview = None
        turbo = self.config.get("turbo", True)
        width, height = fields.get("_size", (1024, 1024))
        if width % 32 or height % 32:
            raise ValueError("Qwen-Image 2.1 native size must be divisible by 32")
        sample = {"sample_method": "euler", "sample_steps": 8 if turbo else fields["_steps"],
                  "guidance": {"txt_cfg": 1.0 if turbo else fields["_cfg"]}}
        if turbo:
            if "num_inference_steps" in fields and int(fields["num_inference_steps"]) != 8:
                raise ValueError("Turbo requires its fixed 8-step sigma schedule")
            if "true_cfg_scale" in fields and float(fields["true_cfg_scale"]) != 1:
                raise ValueError("Turbo requires true_cfg_scale=1")
            sample["custom_sigmas"] = SIGMAS
        if "guidance_scale" in fields:
            sample["guidance"]["distilled_guidance"] = float(fields["guidance_scale"])
        self.load()
        job = self.request("/sdcpp/v1/img_gen", {
            "prompt": fields["prompt"], "negative_prompt": fields.get("negative_prompt", ""),
            "width": width, "height": height, "batch_count": 1, "seed": int(fields.get("seed", -1)),
            "ref_images": [base64.b64encode(data).decode() for data in uploads], "sample_params": sample,
            "output_format": "png", "preview": "proj" if fields["_stream"] else "none",
            "preview_interval": 1})
        identifier = job["id"]
        if not isinstance(identifier, str) or not identifier.replace("_", "").replace("-", "").isalnum():
            raise RuntimeError("Invalid native job ID")
        while True:
            result = self.request("/sdcpp/v1/jobs/" + identifier)
            preview = result.get("preview") or {}
            if preview.get("b64_json"):
                self.preview = preview["b64_json"]
            status = result.get("status")
            if status == "completed":
                data = base64.b64decode(result["result"]["images"][0]["b64_json"], validate=True)
                with Image.open(io.BytesIO(data)) as image:
                    return image.copy()
            if status in ("failed", "cancelled"):
                raise RuntimeError("Native inference failed: " + str(result.get("error")))
            if self.process.poll() is not None:
                raise RuntimeError("Native engine exited during inference")
            time.sleep(.4)


if __name__ == "__main__":
    for stream in (sys.stdin, sys.stdout, sys.stderr):
        stream.reconfigure(encoding="utf-8", errors="replace")
    parser = argparse.ArgumentParser()
    parser.add_argument("--install", action="store_true")
    args = parser.parse_args()
    try:
        config = json.load(sys.stdin)
        install(config) if args.install else verify(config)
    except Exception as error:
        print(f"Native runtime operation failed: {error}", file=sys.stderr, flush=True)
        sys.exit(1)
