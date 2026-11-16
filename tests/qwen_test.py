"""Focused adapter checks; no inference dependencies, downloads or model weights."""
import base64
import http.client
import importlib.util
import json
from pathlib import Path
import threading
import time
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]


def load(name):
    spec = importlib.util.spec_from_file_location(name, ROOT / "plugins/qwen-image" / (name + ".py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


api, environment = load("server"), load("environment")
native, model_info = load("sdcpp_backend"), load("model_info")


class FakeImage:
    mode = "RGB"

    def save(self, file, **kwargs):
        file.write(b"fake-image")


class FakeEngine:
    def __init__(self, mode="generation", retain=True):
        self.config = {"model_id": "test-local", "retain": retain}
        self.pipeline_class, self.mode = "FakePipeline", mode
        self.lock, self.calls, self.unloads = threading.Lock(), [], 0
        self.fail_unload = False

    def status(self):
        return {"loaded": bool(self.calls) and not self.unloads}

    def generate(self, fields, uploads):
        self.calls.append((fields, uploads))
        return FakeImage()

    def unload(self):
        self.unloads += 1
        if self.fail_unload:
            raise RuntimeError("synthetic unload error")


class AdapterTests(unittest.TestCase):
    def setUp(self):
        self.engine = FakeEngine()
        self.httpd = api.Server(("127.0.0.1", 0), api.handler_for(self.engine, "test-secret"))
        self.thread = threading.Thread(target=self.httpd.serve_forever, daemon=True)
        self.thread.start()

    def tearDown(self):
        self.httpd.shutdown()
        self.httpd.server_close()
        self.thread.join()

    def request(self, method="POST", path="/v1/images/generations", body=None, key="test-secret",
                content_type="application/json"):
        connection = http.client.HTTPConnection("127.0.0.1", self.httpd.server_port, timeout=5)
        if isinstance(body, dict):
            body = json.dumps(body).encode()
        connection.request(method, path, body=body, headers={
            "Authorization": "Bearer " + key, "Content-Type": content_type})
        response = connection.getresponse()
        status, data = response.status, response.read()
        connection.close()
        return status, data

    def fields(self, **extra):
        return {"model": "test-local", "prompt": "test", **extra}

    def test_auth_models_json_and_errors(self):
        self.assertEqual(self.request("GET", "/v1/models", key="wrong")[0], 401)
        status, data = self.request("GET", "/v1/models")
        self.assertEqual((status, json.loads(data)["data"][0]["id"]), (200, "test-local"))
        status, data = self.request(body=self.fields(output_format="webp", output_compression=82))
        self.assertEqual(status, 200)
        self.assertEqual(base64.b64decode(json.loads(data)["data"][0]["b64_json"]), b"fake-image")
        self.assertEqual(self.engine.unloads, 0)
        self.assertEqual(self.request(body=b"{broken")[0], 400)
        self.assertEqual(self.request(body=self.fields(model="other"))[0], 404)
        self.assertEqual(self.request(body=self.fields(complexity=1))[0], 400)
        self.assertEqual(self.request(body=self.fields(n=2))[0], 400)
        self.assertEqual(self.request(body=self.fields(size="129x1024"))[0], 400)
        self.assertEqual(self.request(body=self.fields(output_compression=90))[0], 400)

    def test_sse_busy_and_release(self):
        status, data = self.request(body=self.fields(stream=True, partial_images=2))
        self.assertEqual(status, 200)
        self.assertIn(b"image_generation.completed", data)
        self.assertIn(b"[DONE]", data)
        self.engine.lock.acquire()
        try:
            self.assertEqual(self.request(body=self.fields())[0], 429)
            self.assertEqual(self.request(path="/admin/unload")[0], 409)
        finally:
            self.engine.lock.release()
        self.assertEqual(self.request(path="/admin/unload")[0], 200)
        self.engine.config["retain"] = False
        self.request(body=self.fields())
        self.assertEqual(self.engine.unloads, 2)
        # An unload exception must never leave the single inference slot locked.
        self.engine.fail_unload = True
        self.request(body=self.fields())
        self.assertTrue(self.engine.lock.acquire(blocking=False))
        self.engine.lock.release()

    def test_native_partial_stream(self):
        def generation(fields, uploads):
            self.engine.preview = base64.b64encode(b"partial-preview").decode()
            time.sleep(1.2)
            return FakeImage()
        self.engine.generate = generation
        status, data = self.request(body=self.fields(stream=True, partial_images=1))
        self.assertEqual(status, 200)
        self.assertIn(b"image_generation.partial_image", data)
        self.assertIn(base64.b64encode(b"partial-preview"), data)
        self.assertIn(b"image_generation.completed", data)

    def test_multi_reference_edit(self):
        self.engine.mode = "edit-plus"
        boundary = "test-boundary"
        parts = []
        for name, data in (("model", b"test-local"), ("prompt", b"edit"),
                           ("image[]", b"first"), ("image[]", b"second"), ("image[]", b"style")):
            parts.append(b"--" + boundary.encode() + b"\r\nContent-Disposition: form-data; name=\"" +
                         name.encode() + b"\"\r\n\r\n" + data + b"\r\n")
        body = b"".join(parts) + b"--" + boundary.encode() + b"--\r\n"
        kwargs = {"path": "/v1/images/edits", "body": body,
                  "content_type": "multipart/form-data; boundary=" + boundary}
        self.assertEqual(self.request(**kwargs)[0], 200)
        self.assertEqual(self.engine.calls[-1][1], [b"first", b"second", b"style"])
        self.engine.mode = "edit"
        self.assertEqual(self.request(**kwargs)[0], 400)
        self.engine.mode = "generation"
        self.assertEqual(self.request(**kwargs)[0], 400)


class EnvironmentTests(unittest.TestCase):
    def test_first_native_environment_creation(self):
        import tempfile
        with tempfile.TemporaryDirectory() as temporary:
            calls = []
            def run(args, **kwargs):
                calls.append(args)
                if "venv" in args:
                    scripts = Path(args[-1]) / "Scripts"
                    scripts.mkdir(parents=True)
                    (scripts / "python.exe").touch()
            with patch.object(environment, "ROOT", Path(temporary)), \
                    patch.object(environment, "run", side_effect=run), \
                    patch.object(environment, "selected_engine", return_value="sdcpp"):
                environment.install({})
            self.assertEqual(Path(calls[1][0]), Path(temporary) / ".env/Scripts/python.exe")

    def test_environment_paths(self):
        folder, _ = environment.env_python({})
        self.assertEqual(folder, environment.ROOT / ".env")
        for name in ("..", "../outside", "bad/name", ""):
            with self.assertRaises(ValueError):
                environment.env_python({"env_name": name})
        folder, _ = environment.env_python({"environment": str(ROOT)})
        self.assertEqual(folder, ROOT)

    def test_runtime_selection_and_verification(self):
        class Response:
            def __enter__(self): return self
            def __exit__(self, *args): pass
            def read(self): return b'<a href="cu128/">x</a><a href="cu130/">x</a>'
        with patch.object(environment, "nvidia_cuda", return_value=(12, 9)), \
                patch.object(environment.urllib.request, "urlopen", return_value=Response()):
            self.assertTrue(environment.torch_index({"backend": "auto"}).endswith("/cu128"))
        with patch.object(environment, "nvidia_cuda", return_value=None), \
                patch.object(environment, "adapters", return_value=["Intel Arc A770"]):
            self.assertTrue(environment.torch_index({"backend": "auto"}).endswith("/xpu"))
        with patch.object(environment, "nvidia_cuda", return_value=None), \
                patch.object(environment, "adapters", return_value=["AMD Radeon"]):
            with self.assertRaises(RuntimeError):
                environment.torch_index({"backend": "auto"})
        with self.assertRaises(RuntimeError):
            environment.check_backend({"device": "cpu"}, "cuda")
        environment.check_backend({"device": "cuda", "hip": "custom"}, "existing")

    def test_native_component_diagnosis(self):
        import tempfile
        import struct
        with tempfile.TemporaryDirectory() as temporary:
            folder = Path(temporary)
            def string(value):
                data = value.encode()
                return struct.pack("<Q", len(data)) + data
            # GGUF metadata fixtures classify the encoder, never as a diffusion model.
            (folder / "encoder.gguf").write_bytes(
                b"GGUF" + struct.pack("<IQQ", 3, 0, 1) +
                string("general.architecture") + struct.pack("<I", 8) + string("qwen3vl"))
            (folder / "mmproj.gguf").write_bytes(b"fixture")
            result = model_info.describe({"model_directory": temporary})
            self.assertFalse(result["supported"])
            self.assertEqual(result["missing"], ["diffusion_model", "vae"])
            self.assertEqual(Path(result["components"]["llm"]).name, "encoder.gguf")

    @unittest.skipUnless(__import__("os").name == "nt", "Windows Job Object")
    def test_bridge_crash_cleanup(self):
        import ctypes
        import subprocess
        import sys
        import tempfile
        with tempfile.TemporaryDirectory() as temporary:
            marker = Path(temporary) / "child.txt"
            code = (
                "import sys,subprocess,os; from pathlib import Path; "
                "sys.path.insert(0,sys.argv[1]); from sdcpp_backend import CrashJob; "
                "p=subprocess.Popen([sys.executable,'-c','import time;time.sleep(60)']); "
                "job=CrashJob(p); Path(sys.argv[2]).write_text(str(p.pid)); os._exit(23)")
            process = subprocess.run([sys.executable, "-I", "-c", code,
                                      str(ROOT / "plugins/qwen-image"), str(marker)], timeout=10)
            self.assertEqual(process.returncode, 23)
            child_pid = int(marker.read_text())
            kernel = ctypes.WinDLL("kernel32", use_last_error=True)
            kernel.OpenProcess.restype = ctypes.c_void_p
            kernel.WaitForSingleObject.argtypes = [ctypes.c_void_p, ctypes.c_ulong]
            kernel.CloseHandle.argtypes = [ctypes.c_void_p]
            handle = kernel.OpenProcess(0x00100000, False, child_pid)
            if handle:
                try:
                    self.assertEqual(kernel.WaitForSingleObject(handle, 3000), 0)
                finally:
                    kernel.CloseHandle(handle)

    def test_native_request_mapping(self):
        import io
        import types
        engine = native.NativeEngine.__new__(native.NativeEngine)
        engine.config, engine.paths, engine.preview = {"turbo": True}, {"llm_vision": Path("test")}, None
        engine.process = types.SimpleNamespace(poll=lambda: None)
        engine.load = lambda: None
        requests = []
        def request(path, data=None, **kwargs):
            requests.append((path, data))
            if path.endswith("/img_gen"):
                return {"id": "job_test"}
            return {"status": "completed", "result": {"images": [{"b64_json": "YWJj"}]}}
        engine.request = request
        fake_image = types.SimpleNamespace(copy=lambda: FakeImage(), __enter__=lambda self: self)
        class OpenImage:
            def __enter__(self): return fake_image
            def __exit__(self, *args): pass
        from unittest.mock import MagicMock
        pil = MagicMock()
        pil.Image.open.return_value = OpenImage()
        import sys
        with patch.dict(sys.modules, {"PIL": pil}):
            result = engine.generate({"prompt": "test", "_steps": 40, "_cfg": 4, "_stream": True}, [])
        body = requests[0][1]
        self.assertEqual(body["sample_params"]["custom_sigmas"], native.SIGMAS)
        self.assertEqual(body["sample_params"]["guidance"]["txt_cfg"], 1)
        self.assertEqual(body["preview"], "proj")
        self.assertIsInstance(result, FakeImage)


if __name__ == "__main__":
    unittest.main()
