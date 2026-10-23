#!/usr/bin/env python3
"""selftest_verify.py -- prova che verify_cia.py SEGNALA i guasti (mutazioni su una copia di un figlio valido).
Uso: selftest_verify.py CHILD.cia ROMFS_DIR   (CHILD.cia gia' finalizzato e valido)"""
import hashlib, os, struct, subprocess, sys, tempfile
HERE = os.path.dirname(os.path.abspath(__file__))
src, romdir = sys.argv[1], sys.argv[2]
good = bytearray(open(src, "rb").read())
al = lambda x, a=64: (x + a - 1) & ~(a - 1)
hsz, _, _, cs, ts, ms, mt = struct.unpack_from("<IHHIIII", good, 0)
ot = al(hsz) + al(cs); om = ot + al(ts); oc = om + al(ms)
hb = om + 4 + 0x100 + 0x3C
def rehash_tmd(d):                       # ripara SOLO la catena TMD (cosi' il guasto resta visibile solo ai livelli sotto)
    cr, info = hb + 0x9C4, hb + 0xC4
    size = struct.unpack_from(">Q", d, cr + 8)[0]
    d[cr + 0x10:cr + 0x30] = hashlib.sha256(bytes(d[oc:oc + size])).digest()
    for k in range(64):
        f, c = struct.unpack_from(">HH", d, info + 0x24 * k)
        if c: d[info + 0x24 * k + 4:info + 0x24 * k + 0x24] = hashlib.sha256(bytes(d[cr + 0x30 * f:cr + 0x30 * (f + c)])).digest()
    d[hb + 0xA4:hb + 0xC4] = hashlib.sha256(bytes(d[info:info + 0x24 * 64])).digest()
rf_off = struct.unpack_from("<I", good, oc + 0x1B0)[0] * 0x200
rf_sz = struct.unpack_from("<I", good, oc + 0x1B4)[0] * 0x200
fx_off = struct.unpack_from("<I", good, oc + 0x1A0)[0] * 0x200
cases = []
def mut(name, fn, expect):
    d = bytearray(good); fn(d); cases.append((name, d, expect))
mut("1 un byte nei dati RomFS (TMD NON riparato)", lambda d: d.__setitem__(oc + rf_off + 0x1800, d[oc + rf_off + 0x1800] ^ 1), "content 0")
def m2(d): d[oc + rf_off + 0x1800] ^= 1; rehash_tmd(d)
mut("2 un byte nei dati RomFS, TMD ricalcolato (solo IVFC puo' vederlo)", m2, "dei blocchi di L3")
def m3(d): d[oc + fx_off + 0x300] ^= 1; rehash_tmd(d)
mut("3 un byte nell'ExeFS, TMD ricalcolato", m3, "hash")
def m4(d): d[hb + 0x9D] ^= 1                       # versione TMD != ticket
mut("4 versione TMD diversa da quella del ticket", m4, "versione TMD")
def m5(d): d[oc + 0x18F] = 0x05; rehash_tmd(d)     # flag come l'uscita di makerom (NoCrypto) ma contenuto cifrato
mut("5 flags[7]=0x05 su contenuto cifrato", m5, "hash exheader")
def m6(d): d[oc + 0x160] ^= 1; rehash_tmd(d)       # hash exheader sbagliato
mut("6 hash exheader nell'intestazione NCCH", m6, "hash exheader")
def m7(d): d[oc + 0x108] ^= 1; rehash_tmd(d)       # partition id sbagliato -> contatore AES sbagliato
mut("7 partition id alterato", m7, "partition id")
bad = 0
for name, d, expect in cases:
    with tempfile.NamedTemporaryFile(suffix=".cia", delete=False) as f: f.write(bytes(d)); p = f.name
    r = subprocess.run([sys.executable, os.path.join(HERE, "verify_cia.py"), p, "--kind", "child", "--romfs-dir", romdir], capture_output=True, text=True)
    os.unlink(p)
    fails = [l for l in r.stdout.splitlines() if l.startswith("[FAIL]")]
    hit = any(expect in l for l in fails)
    print(("OK   " if (r.returncode == 1 and hit) else "MALE ") + name + f"  -> {len(fails)} FAIL, atteso '{expect}': {'trovato' if hit else 'ASSENTE'}")
    bad += not (r.returncode == 1 and hit)
sys.exit(1 if bad else 0)
