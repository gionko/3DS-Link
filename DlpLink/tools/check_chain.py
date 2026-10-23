#!/usr/bin/env python3
"""check_chain.py -- i controlli della catena di build che esistono perche' 3DS Link e' UN CIA solo: l'app porta il MODELLO del figlio (il figlio senza il suo RomFS), il PLUGIN
(AzaharLink.3gx: lo mette l'app sulla SD, e il figlio Download Play si compone da lui) e la guida "Come ottenere boot9.bin"; il figlio Download Play porta la guida; il figlio vero
(quello di makerom) non si distribuisce piu', e' il riferimento dei test.

  check_chain.py model MODELLO.bin --version16 N [--emu]
        il modello passa le regole del compositore (make_model.py --check), porta la versione N nel TMD e nel biglietto e dichiara la variante giusta.
  check_chain.py agree A.bin B.bin
        due modelli sono LO STESSO modello tranne i campi che makerom tira a sorte a ogni esecuzione (chiave e id del biglietto, le tre firme: biglietto, TMD,
        intestazione NCCH). E' la prova del ciclo: il modello nasce dal figlio SENZA il suo RomFS, quindi non dipende dal CIA dell'app che il RomFS porta (l'app porta
        il modello, il figlio porta l'app): due figli dello stesso installatore con due app diverse danno modelli uguali fuori da quei campi. Esce con 1 se differiscono
        altrove, e dice dove.
  check_chain.py carries FILE... --in DOVE...
        ogni FILE (il modello, le immagini .t3x della guida) si ritrova, byte per byte, in ogni DOVE: un ELF o un .3dsx (cercato cosi' com'e'), oppure un CIA o un modello
        (il codice dell'ExeFS: decifrato a chiave zero se serve e decompresso, e' quello che la console esegue). Esce con 1 se uno manca.
  check_chain.py plugin PLUGIN.3gx --version X.Y.Z --copy DATA.3gx --in DOVE...
        il plugin e' UNA risorsa dell'app (3DS Link e' un pacchetto solo: l'app porta AzaharLink.3gx e lo mette lei sulla SD). PLUGIN e' quello costruito da questo giro
        (AzaharLink/AzaharLink.3gx): e' un .3gx e la sua intestazione dice la versione X.Y.Z (quella di AzaharLink/VERSION); COPY (ConfigApp/data/AzaharLink.3gx, la risorsa con cui
        l'app e' collegata) e' IDENTICA (dimensione e SHA-256); e ogni DOVE (ELF, .3dsx, CIA: come per `carries`) ne porta tutti i byte, con dimensione e SHA-256 nel verdetto.

Solo libreria standard (+ `openssl` per AES-CTR, come verify_cia.py). Dentro Docker devkitarm o con Python di Windows.
"""
import argparse
import hashlib
import os
import struct
import subprocess
import sys

sys.dont_write_bytecode = True                  # (nessun __pycache__ dentro DlpLink/tools)
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import make_model as mm  # noqa: E402

FAILED = []


def say(ok, msg):
    print(("OK   " if ok else "FAIL ") + msg)
    if not ok:
        FAILED.append(msg)
    return ok


def sha(b):
    return hashlib.sha256(bytes(b)).hexdigest()


# ------------------------------------------------------------------ the fields that makerom draws at random
def random_fields(m):
    """Where, in a model, the bytes are that makerom draws at random (name, offset, size). Offsets are in the model (header 0x80 + the CIA's own offsets)."""
    info = mm.parse_model(m)
    b = bytes(m[mm.HDR:mm.HDR + info["head"]])
    L = mm.cia_layout(b)
    ts = mm.be32(b, L["ot"])
    ms = mm.be32(b, L["om"])
    tb, hb = L["tb"], L["hb"]
    H = mm.HDR
    out = [("signature of the ticket", H + L["ot"] + 4, mm.SIG[ts][0]),
           ("title key of the ticket", H + tb + 0x7F, 16),
           ("ticket id", H + tb + 0x90, 8),
           ("signature of the TMD", H + L["om"] + 4, mm.SIG[ms][0]),
           ("signature of the NCCH header", H + L["oc"], 0x100),
           ("checksum of the model (it follows all the rest)", mm.HASH_AT, 32)]
    return out


def name_of(fields, off):
    for name, o, n in fields:
        if o <= off < o + n:
            return name
    return None


def cmd_model(a):
    data = open(a.model, "rb").read()
    try:
        info = mm.parse_model(data)
    except ValueError as e:
        say(False, f"{a.model} is not a model the composer takes: {e}")
        return
    say(True, f"{a.model} passes the rules of the composer ({info['size']} bytes, {'emulator' if info['emu'] else 'real'} variant, sha256 {sha(data)[:16]})")
    say(info["version"] == a.version16, f"its TMD version is {info['version']} ({info['version'] >> 10}.{(info['version'] >> 4) & 63}.{info['version'] & 15}); the version of 3DS Link is {a.version16}")
    say(info["emu"] == a.emu, f"it is the {'emulator' if info['emu'] else 'real'} variant, as wanted")
    say(info["tid"] >> 32 == 0x00040001, f"Title ID {info['tid']:016X}: a Download Play child")


def cmd_agree(a):
    A = open(a.a, "rb").read()
    B = open(a.b, "rb").read()
    try:
        mm.parse_model(A)
        mm.parse_model(B)
    except ValueError as e:
        say(False, f"not a model: {e}")
        return
    if len(A) != len(B):
        say(False, f"the two models have another size ({len(A)} and {len(B)} bytes)")
        return
    fields = random_fields(A)
    diff = [i for i in range(len(A)) if A[i] != B[i]]
    stray = [i for i in diff if name_of(fields, i) is None]
    by_name = {}
    for i in diff:
        n = name_of(fields, i)
        if n:
            by_name[n] = by_name.get(n, 0) + 1
    detail = ", ".join(f"{n} {c}" for n, c in sorted(by_name.items())) or "none"
    say(not stray, f"{os.path.basename(a.a)} and {os.path.basename(a.b)} ({len(A)} bytes) differ in {len(diff)} bytes, all of them in the fields makerom draws at random ({detail})"
        if not stray else
        f"the two models differ outside the fields makerom draws at random: {len(stray)} bytes, the first at 0x{stray[0]:X} (of {len(diff)} that differ)")
    say(len(diff) < len(A) // 100, f"the other {len(A) - len(diff)} bytes ({100 * (len(A) - len(diff)) / len(A):.2f}%) are the same: the installer, the descriptor, the icon, the headers")


# ------------------------------------------------------------------ the code of a CIA, as the console runs it
def aes_ctr(key, iv, data):
    try:
        from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
        c = Cipher(algorithms.AES(key), modes.CTR(iv)).encryptor()
        return c.update(bytes(data)) + c.finalize()
    except ImportError:
        pass
    r = subprocess.run(["openssl", "enc", "-aes-128-ctr", "-nopad", "-K", key.hex(), "-iv", iv.hex()], input=bytes(data), stdout=subprocess.PIPE, check=True)
    return r.stdout


def blz_decompress(c):
    """The backward LZ of the 3DS (ExeFS .code with the 'compress' option; ctrtool's lzss_decompress)."""
    size = len(c)
    if size < 8:
        raise ValueError("compressed code too short")
    top_bottom = struct.unpack_from("<I", c, size - 8)[0]
    extra = struct.unpack_from("<I", c, size - 4)[0]
    out = bytearray(c) + bytearray(extra)
    total = size + extra
    pos = total
    idx = size - (top_bottom >> 24)
    stop = size - (top_bottom & 0xFFFFFF)
    while idx > stop:
        idx -= 1
        control = c[idx]
        for _ in range(8):
            if idx <= stop or pos <= 0:
                break
            if control & 0x80:
                if idx < 2:
                    raise ValueError("compressed code is damaged")
                idx -= 2
                seg = c[idx] | (c[idx + 1] << 8)
                n = ((seg >> 12) & 15) + 3
                off = (seg & 0x0FFF) + 2
                if pos < n:
                    raise ValueError("compressed code is damaged")
                for _ in range(n):
                    if pos + off >= total:
                        raise ValueError("compressed code is damaged")
                    byte = out[pos + off]
                    pos -= 1
                    out[pos] = byte
            else:
                if pos < 1:
                    raise ValueError("compressed code is damaged")
                idx -= 1
                pos -= 1
                out[pos] = c[idx]
            control = (control << 1) & 0xFF
    return bytes(out)


def code_of_cia(d):
    """The .code of the first content of a CIA (or of the head of a model: the ExeFS is all in it), decrypted (a child: key zero) and decompressed."""
    L = mm.cia_layout(d)
    n = L["oc"]
    if d[n + 0x100:n + 0x104] != b"NCCH":
        raise ValueError("no NCCH")
    flags = d[n + 0x188:n + 0x190]
    pid = bytes(reversed(bytes(d[n + 0x108:n + 0x110])))
    ex_off, ex_size = struct.unpack_from("<II", d, n + 0x1A0)
    ex_off, ex_size = ex_off * 0x200, ex_size * 0x200
    exefs = bytes(d[n + ex_off:n + ex_off + ex_size])
    exheader = bytes(d[n + 0x200:n + 0x200 + 0x800])
    if not flags[7] & 4:                                          # not NoCrypto: the children are sealed with the key of zeros
        if not flags[7] & 1:
            raise ValueError("the NCCH is sealed with a key that is not the one of zeros")
        key = bytes(16)
        exefs = aes_ctr(key, pid + bytes([2]) + bytes(7), exefs)
        exheader = aes_ctr(key, pid + bytes([1]) + bytes(7), exheader)
    code = None
    for i in range(10):
        name = exefs[16 * i:16 * i + 8].rstrip(b"\0")
        o, s = struct.unpack_from("<II", exefs, 16 * i + 8)
        if name == b".code":
            code = exefs[0x200 + o:0x200 + o + s]
    if code is None:
        raise ValueError("no .code in the ExeFS")
    return blz_decompress(code) if exheader[0xD] & 1 else code


def searchable(path):
    """(what to search in, how it is described): an ELF or a .3dsx as it is; a CIA or a model as the code the console runs."""
    d = open(path, "rb").read()
    if d[:4] == b"DLPM":
        info = mm.parse_model(d)
        return code_of_cia(d[mm.HDR:mm.HDR + info["head"]]), "the code of the ExeFS of the model"
    if path.lower().endswith(".cia"):
        return code_of_cia(d), "the code of the ExeFS of the CIA"
    return d, "the file"


def cmd_carries(a):
    wanted = []
    for p in a.files:
        wanted.append((os.path.basename(p), open(p, "rb").read()))
    for where in a.where:
        try:
            hay, what = searchable(where)
        except (ValueError, struct.error) as e:
            say(False, f"{where}: {e}")
            continue
        for name, blob in wanted:
            at = hay.find(blob)
            say(at >= 0, f"{os.path.basename(where)} carries {name} ({len(blob)} bytes) in {what}" + (f", at 0x{at:X}" if at >= 0 else ""))


def plugin_version_of(b):
    """The version a .3gx says in its header (bytes 9..11: revision, minor, major, little endian after the magic and the format number), or None when it is no .3gx."""
    if len(b) < 12 or b[:4] != b"3GX$":
        return None
    return "%d.%d.%d" % (b[11], b[10], b[9])


def cmd_plugin(a):
    build = open(a.plugin, "rb").read()
    found = plugin_version_of(build)
    say(found is not None, f"{a.plugin} is a .3gx ({len(build)} bytes, sha256 {sha(build)})")
    say(found == a.version, f"its header says version {found}; the version of 3DS Link is {a.version}")
    copy = open(a.copy, "rb").read()
    same = copy == build
    say(same, f"{a.copy} is the plugin of this build, the resource the app is linked with ({len(copy)} bytes, sha256 {sha(copy)[:16]})" if same else
        f"{a.copy} is NOT the plugin of this build: {len(copy)} bytes, sha256 {sha(copy)[:16]} against {len(build)} bytes, sha256 {sha(build)[:16]} (the app was linked with another plugin: build_all.sh copies it before the app)")
    for where in a.where:
        try:
            hay, what = searchable(where)
        except (ValueError, struct.error) as e:
            say(False, f"{where}: {e}")
            continue
        at = hay.find(build)
        say(at >= 0, f"{os.path.basename(where)} carries the plugin ({len(build)} bytes, sha256 {sha(build)[:16]}) in {what}" + (f", at 0x{at:X}" if at >= 0 else
            ": the app carries another plugin or none (make the app again, after the plugin)"))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("model")
    p.add_argument("model")
    p.add_argument("--version16", type=int, required=True)
    p.add_argument("--emu", action="store_true")
    p = sub.add_parser("agree")
    p.add_argument("a")
    p.add_argument("b")
    p = sub.add_parser("carries")
    p.add_argument("files", nargs="+")
    p.add_argument("--in", dest="where", nargs="+", required=True)
    p = sub.add_parser("plugin")
    p.add_argument("plugin")
    p.add_argument("--version", required=True)
    p.add_argument("--copy", required=True)
    p.add_argument("--in", dest="where", nargs="+", required=True)
    a = ap.parse_args()
    {"model": cmd_model, "agree": cmd_agree, "carries": cmd_carries, "plugin": cmd_plugin}[a.cmd](a)
    sys.exit(1 if FAILED else 0)


if __name__ == "__main__":
    main()
