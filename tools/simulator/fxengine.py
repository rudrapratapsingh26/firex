"""
The compiled fusion engine, spoken to over a pipe.

Both the scenario simulator (tools/simulator/simulate.py) and the replay harness
(tools/validation/replay.py) drive the real C engine through the line protocol
in firmware/sim/fusion_cli.c, so a 32-bit toolchain and a 64-bit Python stay out
of each other's way. Standard library only.

Configurations are named by the node's own setting keys, in the node's units
(firmware/components/core/src/settings.c), so a value tuned here is a
`config set <key> <value>` on the console:

    engine = Engine({"fx_crit_ms": 15000, "fx_mos_pair": 0})
    result = engine.step(t_ms, scores, valid)

The binary is firmware/host-build/fusion_cli (`make -C firmware sim`), or the
path in the FIREX_FUSION_CLI environment variable.
"""
from __future__ import annotations

import hashlib
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MODALITIES = ["gas", "smoke", "flame", "heat", "humidity"]
STATES = ["WARMUP", "NORMAL", "ELEVATED", "CRITICAL"]

# (setting key, node default, engine field, divisor) in fusion_cli's config order.
SETTINGS: list[tuple[str, int, str, int]] = [
    ("fx_enter", 60, "enter", 100),
    ("fx_exit", 40, "exit", 100),
    ("fx_elev_ms", 5000, "elevated_ms", 1),
    ("fx_crit_ms", 10000, "critical_ms", 1),
    ("fx_flame_alone", 1, "flame_alone", 1),
    ("fx_flame_score", 90, "flame_alone_score", 100),
    ("fx_flame_ms", 3000, "flame_alone_ms", 1),
    ("fx_warmup_s", 180, "warmup_ms", 1),  # seconds on the node, milliseconds on the wire
    ("fx_rh_ctx", 75, "rh_context", 100),
    ("fx_heat_quiet", 30, "heat_quiet", 100),
    ("fx_gas_supp", 50, "gas_suppress", 100),
    ("fx_mos_pair", 1, "mos_pair", 1),
]
DEFAULT_SETTINGS: dict[str, int] = {key: default for key, default, _, _ in SETTINGS}
DHT22_FLOOR_MS = 2000


class EngineNotBuilt(FileNotFoundError):
    pass


def binary_path() -> Path:
    override = os.environ.get("FIREX_FUSION_CLI")
    if override:
        return Path(override)
    return ROOT / "firmware" / "host-build" / ("fusion_cli.exe" if sys.platform == "win32" else "fusion_cli")


def available() -> bool:
    return binary_path().exists()


def binary_sha256() -> str | None:
    path = binary_path()
    if not path.exists():
        return None
    return hashlib.sha256(path.read_bytes()).hexdigest()


def merged(settings: dict | None) -> dict[str, int]:
    """Node defaults with overrides applied. Unknown keys are refused, not ignored."""
    result = dict(DEFAULT_SETTINGS)
    for key, value in (settings or {}).items():
        if key not in result:
            raise KeyError(f"unknown fusion setting {key}; known: {', '.join(DEFAULT_SETTINGS)}")
        result[key] = int(value)
    return result


def check_settings(settings: dict | None) -> str | None:
    """Why the node would refuse this set (fx_settings_consistent), or None if it is consistent."""
    s = merged(settings)
    if s["fx_exit"] >= s["fx_enter"]:
        return "fx_exit must be below fx_enter"
    if s["fx_elev_ms"] < DHT22_FLOOR_MS or s["fx_crit_ms"] < DHT22_FLOOR_MS or s["fx_crit_ms"] < s["fx_elev_ms"]:
        return "fusion dwells must be at least the 2 s DHT22 floor, and fx_crit_ms at least fx_elev_ms"
    if s["fx_flame_alone"] and s["fx_flame_ms"] < DHT22_FLOOR_MS:
        return "fx_flame_ms must be at least the 2 s DHT22 floor"
    return None


def config_line(settings: dict | None) -> str:
    s = merged(settings)
    values = []
    for key, _, field, divisor in SETTINGS:
        value = s[key]
        if field == "warmup_ms":
            values.append(str(value * 1000))
        elif divisor == 1:
            values.append(str(value))
        else:
            values.append(f"{value / divisor:.2f}")
    return "config " + " ".join(values)


class Engine:
    """One engine instance. Its clock is whatever t_ms the caller feeds it; it booted at t = 0."""

    CHUNK = 32  # lines per round trip: small enough that neither pipe buffer fills

    def __init__(self, settings: dict | None = None):
        exe = binary_path()
        if not exe.exists():
            raise EngineNotBuilt(f"fusion engine not built at {exe}: run `make -C firmware sim` "
                                 "or set FIREX_FUSION_CLI")
        self.settings = merged(settings)
        self.proc = subprocess.Popen(
            [str(exe)], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            text=True, bufsize=1,
        )
        reply = self._send(config_line(self.settings))
        if reply != "ok":
            self.close()
            raise ValueError(f"engine refused the configuration: {reply}")

    def _send(self, line: str) -> str:
        assert self.proc.stdin and self.proc.stdout
        self.proc.stdin.write(line + "\n")
        self.proc.stdin.flush()
        return self.proc.stdout.readline().strip()

    @staticmethod
    def _step_line(t_ms: int, scores: list[float], valid: list[bool]) -> str:
        parts = " ".join(f"{s:.4f} {1 if v else 0}" for s, v in zip(scores, valid))
        return f"step {int(t_ms)} {parts}"

    @staticmethod
    def _parse(reply: str) -> dict:
        fields = reply.split()
        if len(fields) != 12:
            raise ValueError(f"engine reply not understood: {reply!r}")
        return {
            "state": int(fields[0]),
            "changed": fields[1] == "1",
            "fault": fields[2] == "1",
            "voters": int(fields[3]),
            "voter_count": int(fields[4]),
            "reason_a": int(fields[5]),
            "reason_b": int(fields[6]),
            "effective": [float(x) for x in fields[7:12]],
        }

    def step(self, t_ms: int, scores: list[float], valid: list[bool]) -> dict:
        return self._parse(self._send(self._step_line(t_ms, scores, valid)))

    def steps(self, samples: list[tuple[int, list[float], list[bool]]]) -> list[dict]:
        """Many samples, a chunk of lines per round trip."""
        assert self.proc.stdin and self.proc.stdout
        results: list[dict] = []
        for i in range(0, len(samples), self.CHUNK):
            chunk = samples[i:i + self.CHUNK]
            self.proc.stdin.write("".join(self._step_line(*s) + "\n" for s in chunk))
            self.proc.stdin.flush()
            for _ in chunk:
                results.append(self._parse(self.proc.stdout.readline().strip()))
        return results

    def close(self) -> None:
        if self.proc.poll() is not None:
            return
        assert self.proc.stdin
        try:
            self.proc.stdin.write("quit\n")
            self.proc.stdin.flush()
        except OSError:
            pass
        self.proc.wait(timeout=5)

    def __enter__(self) -> "Engine":
        return self

    def __exit__(self, *exc) -> None:
        self.close()


def reason_names(result: dict) -> list[str]:
    names = []
    for key in ("reason_a", "reason_b"):
        if result[key] < len(MODALITIES):
            names.append(MODALITIES[result[key]])
    return names
