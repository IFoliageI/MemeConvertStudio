"""Environment preparation only. Never installs drivers, toolkits or model weights."""
import argparse
import importlib.metadata
import json
import os
import platform
from pathlib import Path
import re
import subprocess
import sys
import urllib.request

ROOT = Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT))
from model_info import describe, selected_engine


def run(args, **kwargs):
    print("+", subprocess.list2cmdline([str(x) for x in args]), flush=True)
    subprocess.run([str(x) for x in args], check=True, **kwargs)


def env_python(config):
    existing = config.get("environment", "").strip()
    if existing:
        folder = Path(existing).expanduser().resolve()
    else:
        name = config.get("env_name", ".env")
        if not re.fullmatch(r"[A-Za-z0-9_.-]{1,80}", name) or name in (".", ".."):
            raise ValueError("Invalid virtual environment name")
        folder = ROOT / name
    executable = folder / "Scripts" / "python.exe"
    if not executable.is_file():
        executable = folder / ("python.exe" if os.name == "nt" else "bin/python")
    return folder, executable


def inspect():
    result = {"python": sys.executable, "version": platform.python_version(),
              "machine": platform.machine(), "system": platform.system(), "packages": {}}
    for package in ("torch", "torchvision", "diffusers", "transformers", "accelerate", "Pillow"):
        try:
            result["packages"][package] = importlib.metadata.version(package)
        except importlib.metadata.PackageNotFoundError:
            result["packages"][package] = None
    try:
        import torch
        # Allocation plus arithmetic detects unusable runtime/device combinations.
        device = "cuda" if torch.cuda.is_available() else (
            "xpu" if hasattr(torch, "xpu") and torch.xpu.is_available() else "cpu")
        tensor = torch.ones(4, device=device)
        assert float((tensor * 2).sum().item()) == 8
        result.update(device=device, hip=torch.version.hip, cuda=torch.version.cuda)
        if device != "cpu":
            result["gpu"] = getattr(torch, device).get_device_name(0)
        del tensor
    except Exception as error:
        result.update(device="unavailable", torch_error=str(error))
    return result


def nvidia_cuda():
    try:
        output = subprocess.check_output(["nvidia-smi"], text=True, timeout=15,
                                         stderr=subprocess.STDOUT)
        match = re.search(r"CUDA Version:\s*(\d+)\.(\d+)", output)
        return tuple(map(int, match.groups())) if match else None
    except (OSError, subprocess.SubprocessError):
        return None


def adapters():
    if os.name != "nt":
        return []
    try:
        output = subprocess.check_output(
            ["powershell.exe", "-NoProfile", "-NonInteractive", "-Command",
             "Get-CimInstance Win32_VideoController | Select-Object -ExpandProperty Name"],
            text=True, timeout=20, stderr=subprocess.STDOUT, encoding="utf-8", errors="replace")
        return output.splitlines()
    except (OSError, subprocess.SubprocessError):
        return []


def torch_index(config):
    backend = config.get("backend", "auto")
    if backend not in ("auto", "cpu", "cuda", "xpu", "existing"):
        raise RuntimeError("Select Auto / CPU / CUDA / XPU / Existing for the Diffusers engine")
    if backend == "cpu":
        return "https://download.pytorch.org/whl/cpu"
    maximum = nvidia_cuda()
    if backend == "cuda" or (backend == "auto" and maximum):
        if not maximum:
            raise RuntimeError("NVIDIA runtime was not detected. Install/test the system GPU driver separately.")
        # Query the official wheel repository; choose a CUDA runtime no newer than
        # nvidia-smi's supported version. pip handles Python/architecture wheel tags.
        with urllib.request.urlopen("https://download.pytorch.org/whl/", timeout=30) as response:
            html = response.read().decode("utf-8")
        versions = sorted({int(x) for x in re.findall(r'href=["\'][^"\']*?cu(\d{3})/', html)}, reverse=True)
        ceiling = maximum[0] * 10 + maximum[1]
        for version in versions:
            if version <= ceiling:
                return f"https://download.pytorch.org/whl/cu{version}"
        raise RuntimeError("No compatible official CUDA wheel index found for the installed driver.")
    if backend == "xpu" or (backend == "auto" and any(
            re.search(r"Intel.*(?:Arc|Core.*Ultra)", name, re.I) for name in adapters())):
        return "https://download.pytorch.org/whl/xpu"
    if backend == "existing":
        return None
    names = adapters()
    if backend == "auto" and any(re.search(r"NVIDIA|AMD|Radeon|Intel.*(?:Arc|Ultra)", name, re.I)
                                 for name in names):
        raise RuntimeError("GPU detected but no supported automatic runtime is available. "
                           "For AMD, select an existing working HIP/ROCm PyTorch environment. "
                           "For NVIDIA, test nvidia-smi and the system driver first. "
                           "Select CPU explicitly to use CPU inference. No drivers/toolkits were installed.")
    return "https://download.pytorch.org/whl/cpu"


def probe_python(executable):
    result = subprocess.run([str(executable), "-I", str(Path(__file__).resolve()), "--inspect"],
                            text=True, capture_output=True, check=True, encoding="utf-8")
    return json.loads(result.stdout)


def check_backend(info, backend, index=None):
    if info["device"] == "unavailable":
        raise RuntimeError("PyTorch allocation test failed: " + info.get("torch_error", "unknown error"))
    expected = ("cuda" if backend == "cuda" or index and "/cu" in index and "/cpu" not in index
                else "xpu" if backend == "xpu" or index and index.endswith("/xpu") else None)
    if expected and info["device"] != expected:
        raise RuntimeError(f"Installed {expected} dependencies but the GPU allocation test failed. "
                           "Check the GPU model and system driver; no driver/toolkit changes were made.")


def install(config):
    folder, executable = env_python(config)
    if config.get("environment"):
        if not executable.is_file() or not ((folder / "pyvenv.cfg").exists() or
                                             (folder / "conda-meta").is_dir()):
            raise RuntimeError("Select an existing venv/conda environment directory, not the base Python directory.")
    elif not executable.is_file():
        if folder.exists() and any(folder.iterdir()):
            raise RuntimeError("The target environment folder is non-empty and is not a usable venv.")
        run([sys.executable, "-m", "venv", folder])
        folder, executable = env_python(config)
    if selected_engine(config) == "sdcpp":
        run([executable, "-m", "ensurepip", "--upgrade"])
        run([executable, "-m", "pip", "install", "--upgrade", "pip", "Pillow>=10.4"])
        run([executable, "-I", ROOT / "sdcpp_backend.py", "--install"],
            input=json.dumps(config), text=True, encoding="utf-8")
        run([executable, "-I", Path(__file__).resolve(), "--verify"],
            input=json.dumps(config), text=True, encoding="utf-8")
        return
    info = probe_python(executable)
    print(json.dumps(info, ensure_ascii=False, indent=2), flush=True)
    run([executable, "-m", "ensurepip", "--upgrade"])
    run([executable, "-m", "pip", "install", "--upgrade", "pip"])
    backend = config.get("backend", "auto")
    usable = info["device"] in ("cuda", "xpu")
    requested_existing = backend == "existing"
    preserve = requested_existing or (backend == "auto" and usable)
    index = None
    if not preserve:
        index = torch_index(config)
        if index:
            run([executable, "-m", "pip", "install", "--upgrade", "torch", "torchvision",
                 "--index-url", index])
    elif requested_existing and info["device"] == "unavailable":
        raise RuntimeError("The selected environment has no usable PyTorch. Choose an install backend.")
    elif not info["packages"].get("torchvision"):
        # Infer torchvision's matching release from the installed torch metadata.
        # The constraint prevents pip from replacing a working custom GPU build.
        torch_constraint = ROOT / "runtime-constraints.txt"
        torch_constraint.write_text("torch==" + info["packages"]["torch"] + "\n", encoding="utf-8")
        try:
            run([executable, "-m", "pip", "install", "torchvision", "-c", torch_constraint])
        finally:
            torch_constraint.unlink(missing_ok=True)
    # Install only inference dependencies; do not silently change the working torch build.
    constraints = ROOT / "runtime-constraints.txt"
    versions = subprocess.check_output(
        [str(executable), "-I", "-c",
         "import importlib.metadata as m; print('torch=='+m.version('torch')); "
         "print('torchvision=='+m.version('torchvision'))"], text=True)
    constraints.write_text(versions, encoding="utf-8")
    try:
        run([executable, "-m", "pip", "install", "--upgrade", "-r", ROOT / "requirements.txt",
             "-c", constraints])
    finally:
        constraints.unlink(missing_ok=True)
    check_backend(probe_python(executable), backend, index)
    run([executable, "-I", str(Path(__file__).resolve()), "--verify"],
        input=json.dumps(config), text=True, encoding="utf-8")
    print("Environment ready. No model weights, GPU drivers or system toolkits were installed.", flush=True)


def verify(config):
    if selected_engine(config) == "sdcpp":
        from sdcpp_backend import verify as verify_native
        from PIL import Image
        verify_native(config)
        print("Pillow and native runtime imports verified. Actual GPU/model compatibility is checked at first load.",
              flush=True)
        return
    result = inspect()
    print(json.dumps(result, ensure_ascii=False, indent=2), flush=True)
    if result["device"] == "unavailable":
        raise RuntimeError("PyTorch allocation test failed")
    import diffusers
    from diffusers import QwenImagePipeline, QwenImageEditPipeline, QwenImageEditPlusPipeline
    import transformers
    import accelerate
    import torchvision
    from PIL import Image
    print("Qwen pipelines and inference dependencies imported successfully.", flush=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--inspect", action="store_true")
    parser.add_argument("--verify", action="store_true")
    parser.add_argument("--install", action="store_true")
    parser.add_argument("--model", action="store_true")
    args = parser.parse_args()
    if args.inspect:
        print(json.dumps(inspect(), ensure_ascii=False))
    elif args.verify:
        verify(json.load(sys.stdin))
    elif args.install:
        install(json.load(sys.stdin))
    elif args.model:
        result = describe(json.load(sys.stdin))
        print(json.dumps(result, ensure_ascii=False, indent=2), flush=True)
        if not result["supported"]:
            raise RuntimeError(result["message"])


if __name__ == "__main__":
    for stream in (sys.stdin, sys.stdout, sys.stderr):
        stream.reconfigure(encoding="utf-8", errors="replace")
    try:
        main()
    except Exception as error:
        print(f"Environment operation failed: {error}", file=sys.stderr, flush=True)
        sys.exit(1)
