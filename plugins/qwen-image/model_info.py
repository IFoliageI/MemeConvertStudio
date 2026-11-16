"""Inspect model layouts without loading weights."""
import json
from pathlib import Path
import struct

CLASSES = {"QwenImagePipeline": "generation", "QwenImageEditPipeline": "edit",
           "QwenImageEditPlusPipeline": "edit-plus"}


def architecture(path):
    with Path(path).open("rb") as file:
        def read(count):
            if count < 0 or count > 64 * 1024 * 1024 or file.tell() + count > 64 * 1024 * 1024:
                raise ValueError("GGUF metadata exceeds inspection limit")
            data = file.read(count)
            if len(data) != count:
                raise ValueError("Truncated GGUF header")
            return data

        def unpack(fmt):
            return struct.unpack("<" + fmt, read(struct.calcsize("<" + fmt)))[0]

        def string():
            return read(unpack("Q")).decode("utf-8")

        def skip(kind):
            if kind == 8:
                read(unpack("Q"))
            elif kind == 9:
                subtype, count = unpack("I"), unpack("Q")
                if subtype == 9 or count > 1_000_000:
                    raise ValueError("Invalid GGUF metadata array")
                for _ in range(count):
                    skip(subtype)
            else:
                sizes = {0: 1, 1: 1, 2: 2, 3: 2, 4: 4, 5: 4, 6: 4, 7: 1, 10: 8, 11: 8, 12: 8}
                if kind not in sizes:
                    raise ValueError("Unknown GGUF metadata type")
                read(sizes[kind])

        if read(4) != b"GGUF" or unpack("I") not in (2, 3):
            raise ValueError("Not a GGUF v2/v3 file")
        unpack("Q")
        count = unpack("Q")
        if count > 100000:
            raise ValueError("Too many metadata fields")
        for _ in range(count):
            key, kind = string(), unpack("I")
            if key == "general.architecture" and kind == 8:
                return string()
            skip(kind)
        return ""


def selected_engine(config):
    engine = config.get("engine", "auto")
    if engine not in ("auto", "diffusers", "sdcpp"):
        raise ValueError("Unsupported inference engine")
    if engine == "auto":
        return "diffusers" if (Path(config.get("model_directory", "")) / "model_index.json").is_file() else "sdcpp"
    return engine


def components(config):
    folder = Path(config["model_directory"]).expanduser().resolve()
    if not folder.is_dir():
        raise ValueError("Model directory does not exist")
    result = {}
    for name in ("diffusion_model", "vae", "llm", "llm_vision"):
        value = config.get(name, "").strip()
        if value:
            path = Path(value)
            result[name] = (folder / path).resolve() if not path.is_absolute() else path.resolve()
    # Only select a unique candidate; ambiguous choices must be explicit.
    candidates = {name: [] for name in ("diffusion_model", "vae", "llm", "llm_vision")}
    for file in list(folder.glob("*.gguf")) + list(folder.glob("*.safetensors")):
        name = file.name.lower()
        if "mmproj" in name:
            candidates["llm_vision"].append(file)
        elif "vae" in name:
            candidates["vae"].append(file)
        elif file.suffix.lower() == ".gguf":
            arch = architecture(file)
            role = "llm" if arch.startswith(("qwen2vl", "qwen3vl", "qwen2", "qwen3")) else "diffusion_model"
            candidates[role].append(file)
        elif "text" in name or "qwen3vl" in name:
            candidates["llm"].append(file)
        else:
            candidates["diffusion_model"].append(file)
    for role, paths in candidates.items():
        if role not in result and len(paths) == 1:
            result[role] = paths[0].resolve()
    missing = [role for role in ("diffusion_model", "vae", "llm")
               if role not in result or not result[role].is_file()]
    invalid = [role for role, path in result.items() if not path.is_file()]
    return result, sorted(set(missing + invalid))


def describe(config):
    folder = Path(config["model_directory"]).expanduser().resolve()
    if selected_engine(config) == "diffusers":
        model = json.loads((folder / "model_index.json").read_text(encoding="utf-8"))
        cls = model.get("_class_name")
        if cls not in CLASSES:
            raise ValueError(f"Unsupported Diffusers pipeline: {cls}")
        return {"supported": True, "engine": "diffusers", "pipeline": cls, "mode": CLASSES[cls]}
    paths, missing = components(config)
    return {"supported": not missing, "engine": "sdcpp", "components": {k: str(v) for k, v in paths.items()},
            "missing": missing, "message": ("Missing/ambiguous Qwen model components: " + ", ".join(missing) +
            ". Text encoder and mmproj alone cannot generate images. Select the matching diffusion model "
            "and VAE in GGUF settings.") if missing else "Components found; actual compatibility is checked at load time."}
