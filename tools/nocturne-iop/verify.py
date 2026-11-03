#!/usr/bin/env python3
"""Readonly original-IRX integration check. Extracted test assets are temporary."""
import argparse
import array
import hashlib
import json
import math
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import wave


def sha(data):
    return hashlib.sha256(data).hexdigest()


def title_bank(disc):
    ddt = (disc / "DDS3.DDT").read_bytes()
    seen = set()

    def walk(offset, count, depth=0):
        if depth > 16 or (offset, count) in seen:
            raise ValueError("DDT directory loop or excessive depth")
        seen.add((offset, count))
        if offset > len(ddt) or count > (len(ddt) - offset) // 12:
            raise ValueError("DDT directory outside table")
        for i in range(count):
            name_at, sector, size = struct.unpack_from("<IIi", ddt, offset + i * 12)
            if name_at >= len(ddt):
                raise ValueError("DDT name outside table")
            name = ddt[name_at:ddt.index(0, name_at)]
            if size < 0:
                result = walk(sector, -size, depth + 1)
                if result:
                    return result
            elif name.upper() == b"MIDI003A.SMG":
                if size > 2 * 1024 * 1024:
                    raise ValueError("Unexpected title bank size")
                with (disc / "DDS3.IMG").open("rb") as image:
                    image.seek(sector * 2048)
                    data = image.read(size)
                if len(data) != size:
                    raise ValueError("Truncated bank")
                return data
        return None

    _, offset, count = struct.unpack_from("<IIi", ddt)
    if count >= 0:
        raise ValueError("Missing root directory")
    data = walk(offset, -count)
    if not data:
        raise ValueError("MIDI003A.SMG absent")
    return data


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("probe", type=Path)
    parser.add_argument("disc", type=Path)
    parser.add_argument("--wav", type=Path)
    parser.add_argument("--report", type=Path)
    args = parser.parse_args()
    elf = (args.disc / "SLUS_209.11").read_bytes()
    if hashlib.sha1(elf).hexdigest() != "5facfbf7916836613449c922e26a4ec412207b32":
        raise ValueError("This check is validated only against the matching USA ELF")
    bank = title_bank(args.disc)
    with tempfile.TemporaryDirectory(prefix="magatama-native-audio-") as tmp:
        tmp = Path(tmp)
        smg = tmp / "private-title.smg"
        smg.write_bytes(bank)
        outputs = []
        for n in range(2):
            wav = tmp / f"render-{n}.wav"
            result = subprocess.run([str(args.probe.resolve()), str(args.disc.resolve()), str(smg), str(wav)],
                                    capture_output=True, text=True)
            print(result.stdout, end="")
            print(result.stderr, end="", file=sys.stderr)
            if result.returncode:
                raise RuntimeError(f"Original driver probe failed: {result.returncode}")
            outputs.append(wav.read_bytes())
        if outputs[0] != outputs[1]:
            raise AssertionError("Two identical original-driver runs differ")
        # Exercise the original driver's error path, not a host decoder's.
        bad = tmp / "invalid.smg"
        bad.write_bytes(bytes(176))
        result = subprocess.run([str(args.probe.resolve()), str(args.disc.resolve()), str(bad)],
                                capture_output=True, text=True)
        print("INVALID_BANK_EXPECTED_REJECTION", result.returncode)
        print(result.stdout, end="")
        if result.returncode != 1 or "loaded=0" not in result.stdout or "errors=1" not in result.stdout:
            raise AssertionError("Original IRX did not reject the invalid bank as expected")
        with wave.open(str(tmp / "render-0.wav")) as wav:
            frames, channels, rate, width = wav.getnframes(), wav.getnchannels(), wav.getframerate(), wav.getsampwidth()
            pcm = array.array("h", wav.readframes(frames))
        if sys.byteorder != "little":
            pcm.byteswap()
        if (frames, channels, rate, width) != (240000, 2, 48000, 2):
            raise AssertionError("Wrong PCM format or duration")
        peak = max(abs(v) for v in pcm)
        rms = math.sqrt(sum(v * v for v in pcm) / len(pcm))
        if peak <= 128 or rms <= 32:
            raise AssertionError("PCM is effectively silent")
        report = {
            "scope": "headless original-driver render; EE game integration and listening quality unverified",
            "backend": "DQ8 pinned R3000A/IRX/kernel/SPU2 core with Magatama SIF extensions",
            "elf_sha256": sha(elf), "private_bank_sha256": sha(bank),
            "frames": frames, "rate": rate, "channels": channels, "sample_width": width,
            "peak": peak, "rms": rms, "wav_sha256": sha(outputs[0]),
            "repeat_byte_identical": True, "invalid_bank_rejected": True,
            "module_count": 6, "missing_imports": 0, "cpu_faults": 0,
            "modules_sha256": {str(p.relative_to(args.disc)): sha(p.read_bytes())
                               for p in [args.disc / "USERIRX/SDFIOPFL.IRX", args.disc / "USERIRX/SDFSDMAN.IRX",
                                         *[args.disc / "SOUNDIRX" / name for name in
                                           ("LIBSD.IRX", "MODHSYN.IRX", "MODMIDI.IRX", "MODMSIN.IRX")]]},
        }
        if args.wav:
            args.wav.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(tmp / "render-0.wav", args.wav)
        if args.report:
            args.report.parent.mkdir(parents=True, exist_ok=True)
            args.report.write_text(json.dumps(report, indent=2) + "\n")
        print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
