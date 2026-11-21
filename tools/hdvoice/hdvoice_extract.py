#!/usr/bin/env python3
"""Pull Nocturne HD's voice acting out of the player's own HD install, for Magatama's MAGATAMA_HD_VOICE option.

Nothing from either game ships with Magatama. This reads the player's PS2 disc image (DDS3.DDT/DDS3.IMG) and
their HD install, and writes, locally:
  <out>/map.tsv     one line per voiced page: <FNV-1a 64 of the PS2 page text, hex>\t<clip name>
  <out>/<clip>.wav  Japanese,  <out>/<clip>_e.wav  English
HD kept the PS2 event scripts and replaced each text page with a tag like <E601_L0000> naming its voice clip,
so matching the two scripts message by message and page by page gives an exact table.

  python3 hdvoice_extract.py --disc <folder with DDS3.DDT> --hd <smt3hd folder> [--out DIR] [--events e601,e602]
Needs UnityPy (pip install UnityPy).
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

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--disc", required=True); ap.add_argument("--hd", required=True)
    ap.add_argument("--out", default=os.path.expanduser("~/magatama/hdvoice/out"))
    ap.add_argument("--events", default="")
    a = ap.parse_args()
    import UnityPy
    pc = os.path.join(a.hd, "smt3hd_Data", "StreamingAssets", "PC")
    files = ddt_files(a.disc)
    ps2 = {os.path.basename(p)[:-3]: p for p in files if p.startswith("/event/") and p.endswith(".bf")}
    events = [e for e in a.events.split(",") if e] or sorted(n for n in os.listdir(pc) if re.fullmatch(r"e\d{3}", n) and n in ps2)
    os.makedirs(a.out, exist_ok=True)
    img = open(os.path.join(a.disc, "DDS3.IMG"), "rb")
    mp = {}
    mpath = os.path.join(a.out, "map.tsv")
    if os.path.exists(mpath):
        for line in open(mpath):
            k, v = line.rstrip("\n").split("\t"); mp[int(k, 16)] = v
    for ev in events:
        if ev not in ps2 or not os.path.exists(os.path.join(pc, ev)): print(ev, "skipped"); continue
        sec, size = files[ps2[ev]]; img.seek(sec * 2048); pbf = img.read(size)
        env = UnityPy.load(os.path.join(pc, ev))
        hbf, clips = None, {}
        for o in env.objects:
            if o.type.name == "TextAsset":
                d = o.read()
                if d.m_Name.endswith(".bf"):
                    s = d.m_Script; hbf = s if isinstance(s, (bytes, bytearray)) else s.encode("utf-8", "surrogateescape")
            elif o.type.name == "AudioClip":
                clips[o.read().m_Name] = o
        if hbf is None: print(ev, "no HD script"); continue
        P, H = bmd_pages(pbf), bmd_pages(hbf)
        want, pages = set(), 0
        for (pn, pp), (hn, hp) in zip(P, H):
            if pn != hn: continue
            for pg, hg in zip(pp, hp):
                m = TAG.search(hg)
                if not m: continue
                c = m.group(1).decode()
                if c in clips or c + "_e" in clips:
                    mp[key(pg)] = c; want.add(c); pages += 1
        n = 0
        for c in sorted(want):
            for nm in (c, c + "_e"):
                o = clips.get(nm); dst = os.path.join(a.out, nm + ".wav")
                if not o or os.path.exists(dst): continue
                for _, data in o.read().samples.items():
                    open(dst, "wb").write(data); n += 1; break
        print(f"{ev}: {pages} voiced pages, {n} clips written", flush=True)
        with open(mpath, "w") as f:
            for k, v in sorted(mp.items()): f.write(f"{k:016x}\t{v}\n")
    print("map:", len(mp), "pages ->", mpath)

if __name__ == "__main__":
    main()
