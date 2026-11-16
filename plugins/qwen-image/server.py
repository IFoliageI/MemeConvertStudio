"""Small OpenAI-compatible Qwen-Image adapter. Weights always remain local."""
import base64
import gc
import hmac
import io
import json
import math
import os
from pathlib import Path
import socket
import queue
import threading
import time
from email.parser import BytesParser
from email.policy import default
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
from model_info import CLASSES, describe, selected_engine

MAX_BODY = 180 * 1024 * 1024


class ApiError(Exception):
    def __init__(self, message, status=400, param=None):
        super().__init__(message)
        self.status, self.param = status, param


def integer(value, name, minimum, maximum):
    try:
        if isinstance(value, bool) or float(value) != int(value):
            raise ValueError()
        result = int(value)
        if not minimum <= result <= maximum:
            raise ValueError()
        return result
    except (ValueError, TypeError, OverflowError):
        raise ApiError(f"{name} must be an integer in {minimum}..{maximum}", param=name)


def number(value, name, minimum, maximum):
    try:
        result = float(value)
        if not math.isfinite(result) or not minimum <= result <= maximum:
            raise ValueError()
        return result
    except (ValueError, TypeError, OverflowError):
        raise ApiError(f"Invalid {name}", param=name)


def boolean(value, name):
    if value in (True, "true", "True", "1"):
        return True
    if value in (False, "false", "False", "0"):
        return False
    raise ApiError(f"Invalid boolean {name}", param=name)


class Engine:
    def __init__(self, config):
        self.config = config
        folder = Path(config["model_directory"]).expanduser().resolve()
        if not folder.is_dir():
            raise ValueError("Model directory does not exist")
        self.folder = folder
        model = describe(config)
        self.pipeline_class = model["pipeline"]
        self.mode = CLASSES[self.pipeline_class]
        self.pipe = None
        self.device = None
        self.lock = threading.Lock()

    def status(self):
        return {"model": self.config["model_id"], "pipeline": self.pipeline_class,
                "loaded": self.pipe is not None, "retain": self.config.get("retain", True),
                "device": self.device}

    def unload(self):
        self.pipe = None
        gc.collect()
        # Releasing the process is the final fallback for a failed GPU runtime.
        import sys
        torch = sys.modules.get("torch")
        if torch:
            for name in ("cuda", "xpu"):
                module = getattr(torch, name, None)
                if module and module.is_available():
                    module.empty_cache()

    def load(self):
        if self.pipe is not None:
            return
        import torch
        import diffusers
        backend = self.config.get("device", "auto")
        available = "cuda" if torch.cuda.is_available() else (
            "xpu" if hasattr(torch, "xpu") and torch.xpu.is_available() else "cpu")
        device = available if backend == "auto" else backend
        if device not in ("cpu", "cuda", "xpu"):
            raise ApiError("Unsupported device", 503)
        if device != "cpu" and not getattr(torch, device).is_available():
            raise ApiError(f"{device} runtime unavailable; test inference environment first", 503)
        self.device = device
        dtype = torch.float32 if device == "cpu" else torch.bfloat16
        if device == "cuda" and not torch.cuda.is_bf16_supported():
            dtype = torch.float16
        cls = getattr(diffusers, self.pipeline_class)
        pipe = cls.from_pretrained(str(self.folder), torch_dtype=dtype,
                                   local_files_only=True, use_safetensors=True)
        if self.config.get("offload", False) and device != "cpu":
            pipe.enable_model_cpu_offload(device=device)
        else:
            pipe.to(device)
        pipe.set_progress_bar_config(disable=True)
        self.pipe = pipe

    def generate(self, fields, uploads):
        import torch
        from PIL import Image
        kwargs = dict(prompt=fields["prompt"], negative_prompt=fields.get("negative_prompt", " "),
                      num_inference_steps=fields["_steps"], true_cfg_scale=fields["_cfg"],
                      num_images_per_prompt=1)
        if fields.get("size", "auto") != "auto":
            kwargs["width"], kwargs["height"] = fields["_size"]
        if "seed" in fields:
            kwargs["generator"] = torch.Generator(device="cpu").manual_seed(
                integer(fields["seed"], "seed", 0, 2**63 - 1))
        if "guidance_scale" in fields:
            kwargs["guidance_scale"] = number(fields["guidance_scale"], "guidance_scale", 0, 30)
        elif self.mode == "edit-plus":
            kwargs["guidance_scale"] = 1.0
        images = []
        for data in uploads:
            try:
                with Image.open(io.BytesIO(data)) as image:
                    if image.width * image.height > 32_000_000:
                        raise ApiError("Input image exceeds 32 megapixels")
                    images.append(image.convert("RGB"))
            except (OSError, ValueError, Image.DecompressionBombError):
                raise ApiError("Invalid or oversized input image")
        if images:
            kwargs["image"] = images[0] if self.mode == "edit" else images
        self.load()
        with torch.inference_mode():
            return self.pipe(**kwargs).images[0]


def validate(fields, uploads, engine, edit):
    allowed = {"model", "prompt", "n", "size", "quality", "stream", "partial_images", "response_format",
               "output_format", "output_compression", "num_inference_steps", "true_cfg_scale",
               "negative_prompt", "guidance_scale", "seed"}
    unknown = set(fields) - allowed
    if unknown:
        raise ApiError(f"Unsupported parameters: {', '.join(sorted(unknown))}")
    if fields.get("model") != engine.config["model_id"]:
        raise ApiError("Unknown model", 404, "model")
    if not isinstance(fields.get("prompt"), str) or not fields["prompt"].strip():
        raise ApiError("prompt is required", param="prompt")
    if len(fields["prompt"]) > 32000:
        raise ApiError("Prompt too long", param="prompt")
    if integer(fields.get("n", 1), "n", 1, 1) != 1:
        raise ApiError("Only n=1 is supported")
    if fields.get("response_format", "b64_json") != "b64_json":
        raise ApiError("Only response_format=b64_json is supported")
    if edit:
        if not uploads or len(uploads) > 16:
            raise ApiError("Provide 1..16 images")
        if engine.mode == "generation":
            raise ApiError("This is a text-to-image model; select Qwen-Image-Edit/2509/2511 for image edits")
        if engine.mode == "edit" and len(uploads) > 1:
            raise ApiError("This model accepts one image. Multiple references require an EditPlus model.")
    elif engine.mode not in ("generation", "hybrid"):
        raise ApiError("This edit model requires reference images via /images/edits")
    size = fields.get("size", "auto")
    if size != "auto":
        try:
            w, h = map(int, size.split("x"))
        except (ValueError, AttributeError):
            raise ApiError("Invalid size", param="size")
        if min(w, h) < 128 or max(w, h) > 2048 or w * h > 4_194_304 or w % 16 or h % 16:
            raise ApiError("Size must be 128..2048, multiples of 16 and at most 4 megapixels", param="size")
        fields["_size"] = w, h
    quality = fields.get("quality", "auto")
    steps = {"low": 20, "medium": 35, "high": 50, "auto": engine.config.get("steps", 40)}
    if not isinstance(quality, str) or quality not in steps:
        raise ApiError("quality must be low, medium, high or auto", param="quality")
    fields["_steps"] = integer(fields.get("num_inference_steps", steps[quality]), "num_inference_steps", 1, 100)
    fields["_cfg"] = number(fields.get("true_cfg_scale", engine.config.get("cfg", 4)), "true_cfg_scale", 1, 30)
    fmt = fields.get("output_format", "png")
    if fmt not in ("png", "jpeg", "webp"):
        raise ApiError("Unsupported output_format", param="output_format")
    fields["_compression"] = integer(fields.get("output_compression", 90), "output_compression", 0, 100)
    if "output_compression" in fields and fmt == "png":
        raise ApiError("output_compression only applies to jpeg/webp")
    fields["_stream"] = boolean(fields.get("stream", False), "stream")
    if "partial_images" in fields:
        integer(fields["partial_images"], "partial_images", 1, 3)
    # All remaining options are validated before loading the model.
    if "seed" in fields:
        integer(fields["seed"], "seed", 0, 2**63 - 1)
    if "guidance_scale" in fields:
        number(fields["guidance_scale"], "guidance_scale", 0, 30)
    if not isinstance(fields.get("negative_prompt", ""), str):
        raise ApiError("negative_prompt must be a string")


class Server(ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = False

    def server_bind(self):
        if os.name == "nt":
            self.socket.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
        super().server_bind()


def handler_for(engine, key):
    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, format, *args):
            # Never log headers, prompts, images or keys.
            pass

        def setup(self):
            super().setup()
            self.connection.settimeout(60)

        def reply(self, status, data):
            raw = json.dumps(data, ensure_ascii=False).encode("utf-8")
            self.send_response(status)
            self.send_header("Content-Type", "application/json; charset=utf-8")
            self.send_header("Content-Length", str(len(raw)))
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(raw)

        def authorized(self):
            supplied = self.headers.get("Authorization", "")
            if not hmac.compare_digest(supplied.encode("utf-8"), ("Bearer " + key).encode("utf-8")):
                self.reply(401, {"error": {"message": "Invalid API key", "type": "authentication_error"}})
                return False
            return True

        def do_GET(self):
            if not self.authorized():
                return
            if self.path == "/v1/models":
                self.reply(200, {"object": "list", "data": [
                    {"id": engine.config["model_id"], "object": "model", "owned_by": "local-qwen"}]})
            elif self.path == "/health":
                self.reply(200, engine.status())
            else:
                self.reply(404, {"error": {"message": "Not found"}})

        def read_fields(self):
            size = integer(self.headers.get("Content-Length", 0), "Content-Length", 1, MAX_BODY)
            content_type = self.headers.get("Content-Type", "")
            body = self.rfile.read(size)
            if len(body) != size:
                raise ApiError("Incomplete request")
            if content_type.startswith("application/json"):
                try:
                    fields = json.loads(body)
                except (ValueError, UnicodeError):
                    raise ApiError("Invalid JSON body")
                if not isinstance(fields, dict):
                    raise ApiError("JSON body must be an object")
                return fields, []
            if not content_type.startswith("multipart/form-data"):
                raise ApiError("Use application/json or multipart/form-data")
            message = BytesParser(policy=default).parsebytes(
                f"Content-Type: {content_type}\r\nMIME-Version: 1.0\r\n\r\n".encode("ascii") + body)
            if not message.is_multipart():
                raise ApiError("Invalid multipart body")
            fields, images = {}, []
            for part in message.iter_parts():
                name = part.get_param("name", header="content-disposition")
                raw = part.get_payload(decode=True)
                if name in ("image", "image[]"):
                    if not raw or len(raw) > 50 * 1024 * 1024 or len(images) >= 16:
                        raise ApiError("Too many images or image exceeds 50 MB")
                    images.append(raw)
                elif name:
                    if raw is None or name in fields or len(raw) > 128000:
                        raise ApiError("Duplicate or oversized field")
                    try:
                        fields[name] = raw.decode("utf-8")
                    except UnicodeError:
                        raise ApiError("Form fields must be UTF-8")
            return fields, images

        def infer(self, fields, uploads, streaming, edit):
            if not streaming:
                return engine.generate(fields, uploads)
            results = queue.Queue(maxsize=1)

            def work():
                try:
                    results.put((engine.generate(fields, uploads), None))
                except BaseException as error:
                    results.put((None, error))

            threading.Thread(target=work, daemon=True).start()
            disconnected = False
            last_preview, preview_count = None, 0
            heartbeat = time.monotonic()
            maximum = int(fields.get("partial_images", 3))
            while True:
                try:
                    image, error = results.get(timeout=1)
                    if error:
                        raise error
                    if disconnected:
                        raise BrokenPipeError()
                    return image
                except queue.Empty:
                    if not disconnected:
                        try:
                            preview = getattr(engine, "preview", None)
                            if preview and preview != last_preview and preview_count < maximum:
                                event = {"type": ("image_edit" if edit else "image_generation") + ".partial_image",
                                         "b64_json": preview, "partial_image_index": preview_count}
                                self.wfile.write(("data: " + json.dumps(event) + "\n\n").encode())
                                last_preview, preview_count = preview, preview_count + 1
                                self.wfile.flush()
                            if time.monotonic() - heartbeat >= 15:
                                self.wfile.write(b": inference in progress\n\n")
                                self.wfile.flush()
                                heartbeat = time.monotonic()
                        except OSError:
                            # Keep the inference lock until the GPU worker finishes.
                            disconnected = True

        def do_POST(self):
            if not self.authorized():
                return
            streaming = False
            try:
                if self.path == "/admin/unload":
                    if self.client_address[0] not in ("127.0.0.1", "::1"):
                        raise ApiError("Administrative requests are local only", 403)
                    if not engine.lock.acquire(blocking=False):
                        raise ApiError("Inference is busy", 409)
                    try:
                        engine.unload()
                    finally:
                        engine.lock.release()
                    self.reply(200, engine.status())
                    return
                if self.path not in ("/v1/images/generations", "/v1/images/edits"):
                    raise ApiError("Not found", 404)
                fields, uploads = self.read_fields()
                edit = self.path.endswith("/edits")
                validate(fields, uploads, engine, edit)
                if not engine.lock.acquire(blocking=False):
                    raise ApiError("Another generation is running", 429)
                try:
                    if fields["_stream"]:
                        self.send_response(200)
                        self.send_header("Content-Type", "text/event-stream")
                        self.send_header("Connection", "close")
                        self.end_headers()
                        streaming = True
                        self.wfile.write(b": local Qwen inference started\n\n")
                        self.wfile.flush()
                    image = self.infer(fields, uploads, streaming, edit)
                    fmt = fields.get("output_format", "png")
                    if fmt == "jpeg" and image.mode not in ("RGB", "L"):
                        image = image.convert("RGB")
                    data = io.BytesIO()
                    image.save(data, format=fmt.upper() if fmt != "jpeg" else "JPEG",
                               **({"quality": fields["_compression"]} if fmt != "png" else {}))
                    encoded = base64.b64encode(data.getvalue()).decode("ascii")
                finally:
                    try:
                        if not engine.config.get("retain", True):
                            engine.unload()
                    finally:
                        engine.lock.release()
                if streaming:
                    event = {"type": ("image_edit" if edit else "image_generation") + ".completed",
                             "b64_json": encoded}
                    self.wfile.write(("data: " + json.dumps(event) + "\n\ndata: [DONE]\n\n").encode())
                    self.wfile.flush()
                else:
                    self.reply(200, {"created": int(time.time()), "data": [{"b64_json": encoded}]})
            except (BrokenPipeError, ConnectionResetError, TimeoutError):
                pass
            except Exception as error:
                status = error.status if isinstance(error, ApiError) else 500
                # Redact key if any dependency happens to include it in an error.
                message = str(error).replace(key, "[REDACTED]")[:1500]
                payload = {"error": {"message": message, "type": "local_inference_error",
                                     "param": getattr(error, "param", None)}}
                if streaming:
                    try:
                        self.wfile.write(("data: " + json.dumps(payload) + "\n\n").encode())
                        self.wfile.flush()
                    except OSError:
                        pass
                else:
                    self.reply(status, payload)
            finally:
                self.close_connection = True

    return Handler


def watch_parent(pid):
    if os.name != "nt":
        return
    import ctypes
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.OpenProcess.restype = ctypes.c_void_p
    handle = kernel.OpenProcess(0x00100000, False, pid)
    if not handle:
        os._exit(1)
    kernel.WaitForSingleObject.argtypes = [ctypes.c_void_p, ctypes.c_ulong]
    kernel.WaitForSingleObject(handle, 0xFFFFFFFF)
    # Exit bypasses potentially broken GPU finalizers on parent death.
    os._exit(0)


def serve(config, engine_factory=None):
    key = config.get("key", "")
    if not isinstance(key, str) or len(key) < 8 or any(ord(c) < 33 or ord(c) > 126 for c in key):
        raise ValueError("API key must contain at least 8 printable ASCII characters")
    if engine_factory is None:
        if selected_engine(config) == "sdcpp":
            from sdcpp_backend import NativeEngine
            engine_factory = NativeEngine
        else:
            engine_factory = Engine
    engine = engine_factory(config)
    host = "0.0.0.0" if config.get("lan", False) else "127.0.0.1"
    port = integer(config.get("port", 8189), "port", 1024, 65535)
    server = Server((host, port), handler_for(engine, key))
    if config.get("parent_pid"):
        threading.Thread(target=watch_parent, args=(config["parent_pid"],), daemon=True).start()
    if config.get("ready_file"):
        Path(config["ready_file"]).write_text(json.dumps({"ready": True, "port": port}), encoding="utf-8")
    print(f"Qwen local API ready on {host}:{port}; pipeline={engine.pipeline_class}", flush=True)
    try:
        server.serve_forever()
    finally:
        server.server_close()
        engine.unload()


if __name__ == "__main__":
    for stream in (sys.stdin, sys.stdout, sys.stderr):
        stream.reconfigure(encoding="utf-8", errors="replace")
    try:
        serve(json.load(sys.stdin))
    except Exception as error:
        print(f"Local Qwen service failed: {error}", file=sys.stderr, flush=True)
        sys.exit(1)
