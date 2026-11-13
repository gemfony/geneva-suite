"""Configuration loading with sensible built-in defaults.

The harness runs with zero configuration; ``config.yaml`` only overrides
defaults. PyYAML is used when present; if it is missing, the built-in defaults
apply and a note is emitted.
"""

from __future__ import annotations

import os
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

try:
    import yaml  # type: ignore
    _HAVE_YAML = True
except ImportError:  # pragma: no cover - exercised only without PyYAML
    yaml = None  # type: ignore
    _HAVE_YAML = False

from .model import BuildType, Compiler, GuestOS

# Built-in defaults. Mirrors config.yaml so the tool is usable with no file.
_DEFAULTS: dict[str, Any] = {
    "source_dir": None,  # inferred from this file's location
    "workdir": "/home/rberlich/Testplan",
    "backend": "podman",
    "boost_root": "/opt/boost",
    "jobs": 1,
    "guests": [
        {"slug": "ubuntu-26.10", "image": "docker.io/library/ubuntu:26.10"},
    ],
    "compilers": ["gcc", "clang"],
    "short_build_types": ["Debug", "Release"],
    "long_build_types": ["RelWithDebInfo", "Sanitize"],
    "feature_sets": {
        "base": {"tests": True, "examples": True, "benchmarks": False,
                 "mpi": False, "cuda": False},
        "minimal": {"tests": False, "examples": False, "benchmarks": False,
                    "mpi": False, "cuda": False},
        "full": {"tests": True, "examples": True, "benchmarks": True,
                 "mpi": True, "cuda": True},
    },
}


@dataclass
class FeatureSet:
    name: str
    tests: bool
    examples: bool
    benchmarks: bool
    mpi: bool
    cuda: bool


@dataclass
class Config:
    source_dir: Path
    workdir: Path
    backend: str
    boost_root: str
    jobs: int
    guests: list[GuestOS]
    compilers: list[Compiler]
    short_build_types: list[BuildType]
    long_build_types: list[BuildType]
    feature_sets: dict[str, FeatureSet] = field(default_factory=dict)
    notes: list[str] = field(default_factory=list)


def _infer_source_dir() -> Path:
    # ReleaseTests/releaseharness/config.py -> repo root is two parents up.
    return Path(__file__).resolve().parents[2]


def load(path: str | os.PathLike[str] | None) -> Config:
    """Load configuration, layering an optional YAML file over the defaults."""
    data = dict(_DEFAULTS)
    notes: list[str] = []

    if path is not None:
        p = Path(path)
        if not p.exists():
            raise FileNotFoundError(f"config file not found: {p}")
        if not _HAVE_YAML:
            notes.append(
                f"PyYAML not available; ignoring {p} and using built-in defaults."
            )
        else:
            with p.open("r", encoding="utf-8") as fh:
                loaded = yaml.safe_load(fh) or {}
            # Shallow merge: top-level keys present in the file override defaults.
            for key, value in loaded.items():
                data[key] = value

    src = data["source_dir"]
    source_dir = Path(src).resolve() if src else _infer_source_dir()

    guests = [
        GuestOS(slug=g["slug"], image=g["image"])
        for g in data["guests"]
    ]
    compilers = [Compiler(c) for c in data["compilers"]]
    short_bt = [BuildType(b) for b in data["short_build_types"]]
    long_bt = [BuildType(b) for b in data["long_build_types"]]

    feature_sets = {
        name: FeatureSet(name=name, **vals)
        for name, vals in data["feature_sets"].items()
    }

    return Config(
        source_dir=source_dir,
        workdir=Path(data["workdir"]).resolve(),
        backend=str(data["backend"]),
        boost_root=str(data["boost_root"]),
        jobs=int(data["jobs"]),
        guests=guests,
        compilers=compilers,
        short_build_types=short_bt,
        long_build_types=long_bt,
        feature_sets=feature_sets,
        notes=notes,
    )
