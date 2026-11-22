#!/usr/bin/env python3
"""Pull Nocturne HD's voice acting out of the player's own HD install, for Magatama's MAGATAMA_HD_VOICE option.

Nothing from either game ships with Magatama. This reads the player's PS2 disc image (DDS3.DDT/DDS3.IMG) and
their HD install, and writes, locally:
  <out>/map.tsv     one line per voiced page: <FNV-1a 64 of the PS2 page text, hex>\t<clip name>
  <out>/<clip>.wav  Japanese,  <out>/<clip>_e.wav  English
HD kept the PS2 event scripts and replaced each text page with a tag like <E601_L0000> naming its voice clip,
so matching the two scripts message by message and page by page gives an exact table.

  python3 hdvoice_extract.py --disc <folder with DDS3.DDT> --hd <smt3hd folder> [--out DIR] [--only e601,f024]
                             [--lang en|ja|both] [--compact]
Covers story events (HD bundles eNNN, PS2 /event/.../eNNN.bf) and field conversations (HD bundles fNNN, PS2
/fld/f/fNNN/fNNN.bf). A script's clips live in HD bundle "e" + the number in their tag (E601_ -> e601, F024_ -> e024).
Clips are stored mono, 16-bit, at HD's rate (half HD's size; --compact: IMA ADPCM, about an eighth, some hiss).
Needs UnityPy (pip install UnityPy); ffmpeg on PATH for the conversion (without it clips are kept as HD has them).
"""
import argparse, os, re, struct, sys

def ddt_files(disc):
    d = open(os.path.join(disc, "DDS3.DDT"), "rb").read(); files = {}
    def name(o): return d[o:d.index(b"\0", o)].decode("latin1").lower()
    def walk(off, cnt, dr):
        for k in range(cnt):
            a, b, c = struct.unpack_from("<IIi", d, off + 12 * k); n = name(a)
            if c < 0: walk(b, -c, dr + "/" + n if n else dr)
            else: files[dr + "/" + n] = (b, c)
    a, b, c = struct.unpack_from("<IIi", d, 0); walk(b, -c, ""); return files

def bmd_pages(bf):
    """[(dialog name, [page bytes...])] from a FLW0 script's message section, in file order."""
    if bf[8:12] != b"FLW0": return []
    sec = {}
    for i in range(struct.unpack_from("<I", bf, 0x10)[0]):
        t, es, c, o = struct.unpack_from("<4I", bf, 0x20 + 16 * i); sec[t] = (es, c, o)
    if 3 not in sec or not sec[3][1]: return []
    es, c, mo = sec[3]; b = bf[mo:mo + es * c]
    if b[8:12] != b"MSG1": return []
    out = []
    for k in range(struct.unpack_from("<I", b, 0x18)[0]):
        kind, off = struct.unpack_from("<II", b, 0x20 + 8 * k)
        h = 0x20 + off; nm = b[h:h + 24].split(b"\0")[0].decode("latin1")
        if kind != 0: out.append((nm, [])); continue
        lc = struct.unpack_from("<H", b, h + 24)[0]
        offs = struct.unpack_from("<%dI" % lc, b, h + 28)
        out.append((nm, [b[0x20 + o:0x20 + o + 64] for o in offs]))
    return out

def key(page):  # must match hdvKey() in src/product/patches.cpp
    h = 1469598103934665603
    for ch in page[:48]:
        if ch == 0: break
        h = ((h ^ ch) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return h

TAG = re.compile(rb"<([A-Z]\d{3}_L\d{4})>")

def convert(data, dst, compact):
    """Write HD's WAV bytes to dst as mono 16-bit PCM (or IMA ADPCM with compact) via ffmpeg; raw copy without it."""
    import shutil, subprocess
    if shutil.which("ffmpeg"):
        codec = "adpcm_ima_wav" if compact else "pcm_s16le"
        r = subprocess.run(["ffmpeg", "-v", "error", "-y", "-f", "wav", "-i", "pipe:0", "-ac", "1", "-c:a", codec, dst],
                           input=data, capture_output=True)
        if r.returncode == 0: return
    open(dst, "wb").write(data)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--disc", required=True); ap.add_argument("--hd", required=True)
    ap.add_argument("--out", default=os.path.expanduser("~/magatama/hdvoice/out"))
    ap.add_argument("--only", "--events", dest="only", default="", help="comma list of HD script bundles (e601,f024)")
    ap.add_argument("--lang", default="both", choices=["en", "ja", "both"])
    ap.add_argument("--compact", action="store_true")
    a = ap.parse_args()
    import UnityPy
    pc = os.path.join(a.hd, "smt3hd_Data", "StreamingAssets", "PC")
    files = ddt_files(a.disc)
    ps2 = {}
    for p in files:
        if p.endswith(".bf") and (p.startswith("/event/") or p.startswith("/fld/")):
            ps2.setdefault(os.path.basename(p)[:-3], p)
    names = [e for e in a.only.split(",") if e] or sorted(
        n for n in os.listdir(pc) if re.fullmatch(r"[ef]\d{3}", n) and n in ps2)
    suffixes = {"en": ["_e"], "ja": [""], "both": ["", "_e"]}[a.lang]
    os.makedirs(a.out, exist_ok=True)
    img = open(os.path.join(a.disc, "DDS3.IMG"), "rb")
    mp = {}
    mpath = os.path.join(a.out, "map.tsv")
    if os.path.exists(mpath):
        for line in open(mpath):
            k, v = line.rstrip("\n").split("\t"); mp[int(k, 16)] = v
    clip_cache = {}
    def clips_of(bundle):
        if bundle not in clip_cache:
            path = os.path.join(pc, bundle); found = {}
            if os.path.exists(path):
                for o in UnityPy.load(path).objects:
                    if o.type.name == "AudioClip": found[o.read().m_Name] = o
            clip_cache.clear(); clip_cache[bundle] = found   # keep one bundle's objects in memory at a time
        return clip_cache[bundle]
    total_pages = total_clips = 0
    for name in names:
        if name not in ps2 or not os.path.exists(os.path.join(pc, name)): print(name, "skipped"); continue
        sec, size = files[ps2[name]]; img.seek(sec * 2048); pbf = img.read(size)
        hbf = None
        for o in UnityPy.load(os.path.join(pc, name)).objects:
            if o.type.name == "TextAsset":
                d = o.read()
                if d.m_Name.lower().endswith(".bf"):
                    s = d.m_Script; hbf = s if isinstance(s, (bytes, bytearray)) else s.encode("utf-8", "surrogateescape")
        if hbf is None: continue
        tagged = []
        for (pn, pp), (hn, hp) in zip(bmd_pages(pbf), bmd_pages(hbf)):
            if pn != hn: continue
            for pg, hg in zip(pp, hp):
                m = TAG.search(hg)
                if m: tagged.append((key(pg), m.group(1).decode()))
        if not tagged: continue
        pages = written = 0
        for k, c in tagged:
            clips = clips_of("e" + c[1:4])
            if not any(c + s in clips for s in suffixes): continue
            mp[k] = c; pages += 1
            for s in suffixes:
                o = clips.get(c + s); dst = os.path.join(a.out, c + s + ".wav")
                if not o or os.path.exists(dst): continue
                for _, data in o.read().samples.items():
                    convert(data, dst, a.compact); written += 1; break
        total_pages += pages; total_clips += written
        print(f"{name}: {pages} voiced pages, {written} clips written", flush=True)
        with open(mpath, "w") as f:
            for k, v in sorted(mp.items()): f.write(f"{k:016x}\t{v}\n")
    print(f"done: {total_pages} voiced pages, {total_clips} clips written; map has {len(mp)} pages -> {mpath}")

if __name__ == "__main__":
    main()
