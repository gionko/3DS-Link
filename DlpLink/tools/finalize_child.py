#!/usr/bin/env python3
"""finalize_child.py v2 -- rifinisce il CIA del figlio DLP uscito da makerom (sostituisce DlpTest/tools/finalize_child.py).

Ingresso: CIA prodotto da
    makerom -f cia -target t -exefslogo -desc dlpchild:1 -elf ... -rsf dlpchild.rsf -icon ... -ver N
Operazioni, nell'ordine (la logica di cifratura e di hash e' quella di DlpTest/tools/finalize_child.py, dimostrata su
hardware il 2026-10-18; cambiano solo le protezioni):
  1. innesta l'Access Descriptor FIRMATO DA NINTENDO dei figli DLP (0x400 byte, programId jolly 00040001FFFFFFFF) letto
     da un file di soli 0x400 byte (data/dlpchild_ad.bin) invece che dai 22 MB del figlio di Tetris; se manca = ERRORE
     (la vecchia versione lo saltava in silenzio se il percorso relativo non esisteva);
  2. (opzionale) --version N: scrive N come versione del titolo nel TMD e nel ticket (makerom -ver perde il campo
     "minor": -ver 1090 produce 1026);
  3. azzera l'id del contenuto, flags[3]=0x00, flags[7]=0x01 (solo FixedCryptoKey, niente NoCrypto);
  4. hash dell'exheader nell'intestazione NCCH (sul CHIARO), poi cifra exheader/ExeFS/RomFS con AES-128-CTR a chiave
     di soli zeri (contatore = partitionId big-endian | sezione | 7 zeri; sezione 1 exheader, 2 ExeFS, 3 RomFS);
  5. ricalcola la catena di hash del TMD sui byte memorizzati (cifrati): contenuto -> info record -> header.
Si rifiuta di girare due volte sullo stesso file (flags[7] senza NoCrypto = gia' finalizzato: doppia cifratura).
Dipendenze: python3 >= 3.8 e il comando `openssl` (come l'originale).
"""
import argparse, hashlib, os, struct, subprocess, sys

SIGSZ = {0x10000: 0x200, 0x10001: 0x100, 0x10002: 0x3C, 0x10003: 0x200, 0x10004: 0x100, 0x10005: 0x3C}
SIGPAD = {0x10000: 0x3C, 0x10001: 0x3C, 0x10002: 0x40, 0x10003: 0x3C, 0x10004: 0x3C, 0x10005: 0x40}


def al(x, a=64):
    return (x + a - 1) & ~(a - 1)


def parts(d):
    """(offset NCCH nel CIA, offset TMD header body, offset ticket body)."""
    hsz, typ, ver, certsz, tiksz, tmdsz, metasz = struct.unpack_from("<IHHIIII", d, 0)
    ot = al(hsz) + al(certsz)
    om = ot + al(tiksz)
    oc = om + al(tmdsz)
    ts = struct.unpack_from(">I", d, ot)[0]
    ms = struct.unpack_from(">I", d, om)[0]
    return oc, om + 4 + SIGSZ[ms] + SIGPAD[ms], ot + 4 + SIGSZ[ts] + SIGPAD[ts]


def aes_ctr(key, ctr, data):
    r = subprocess.run(["openssl", "enc", "-aes-128-ctr", "-nopad", "-K", key.hex(), "-iv", ctr.hex()],
                       input=bytes(data), stdout=subprocess.PIPE, check=True)
    if len(r.stdout) != len(data):
        raise RuntimeError("openssl ha restituito una lunghezza diversa")
    return r.stdout


def ncch_sections(d, oc):
    out = [("exheader", oc + 0x200, 0x800, 1)]
    for name, off_fld, sz_fld, sec in (("ExeFS", 0x1A0, 0x1A4, 2), ("RomFS", 0x1B0, 0x1B4, 3)):
        off = struct.unpack_from("<I", d, oc + off_fld)[0] * 0x200
        sz = struct.unpack_from("<I", d, oc + sz_fld)[0] * 0x200
        if sz:
            out.append((name, oc + off, sz, sec))
    return out


def ncch_crypt(d, oc, key, label):
    pid = bytes(reversed(bytes(d[oc + 0x108:oc + 0x110])))
    for name, off, sz, sec in ncch_sections(d, oc):
        d[off:off + sz] = aes_ctr(key, pid + bytes([sec]) + b"\0" * 7, d[off:off + sz])
        print(f"    {label} {name:8} 0x{off - oc:06X} +0x{sz:06X}  sezione {sec}")


def tmd_hashes(d, oc, hb):
    cr, info = hb + 0x9C4, hb + 0xC4
    n = struct.unpack_from(">H", d, hb + 0x9E)[0]
    pos = oc
    for i in range(n):
        size = struct.unpack_from(">Q", d, cr + 0x30 * i + 8)[0]
        d[cr + 0x30 * i + 0x10:cr + 0x30 * i + 0x30] = hashlib.sha256(bytes(d[pos:pos + size])).digest()
        pos += al(size)
    for k in range(64):
        r = info + 0x24 * k
        first, count = struct.unpack_from(">HH", d, r)
        if count:
            d[r + 4:r + 0x24] = hashlib.sha256(bytes(d[cr + 0x30 * first:cr + 0x30 * (first + count)])).digest()
    d[hb + 0xA4:hb + 0xC4] = hashlib.sha256(bytes(d[info:info + 0x24 * 64])).digest()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cia")
    ap.add_argument("--ad", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "data", "dlpchild_ad.bin"))
    ap.add_argument("--version", type=int, help="versione titolo u16 (TMD + ticket)")
    a = ap.parse_args()

    d = bytearray(open(a.cia, "rb").read())
    oc, hb, tb = parts(d)
    if bytes(d[oc + 0x100:oc + 0x104]) != b"NCCH":
        sys.exit("ERRORE: il contenuto 0 non e' un NCCH")
    if not (d[oc + 0x18F] & 0x04):
        sys.exit("ERRORE: flags[7] senza NoCrypto: il file e' gia' stato finalizzato (o non viene da makerom -target t)")
    if struct.unpack_from("<I", d, oc + 0x1B4)[0] == 0:
        sys.exit("ERRORE: il figlio non ha RomFS (serve la sezione RomFs nell'RSF: flags[5]=0x03)")
    tid = struct.unpack_from("<Q", d, oc + 0x200 + 0x200)[0]
    if (tid >> 32) != 0x00040001:
        sys.exit(f"ERRORE: Title ID {tid:016X} non e' un figlio DLP (0004 0001 ...)")
    if (tid >> 32) & 0x10:
        sys.exit("ERRORE: titolo di sistema: la chiave fissa non e' quella di soli zeri")
    print(f"{a.cia}: Title ID {tid:016X}")

    ad = open(a.ad, "rb").read()
    if len(ad) != 0x400 or struct.unpack_from("<Q", ad, 0x200)[0] != 0x00040001FFFFFFFF:
        sys.exit("ERRORE: --ad non e' l'Access Descriptor jolly dei figli DLP (0x400 byte, programId 00040001FFFFFFFF)")
    e = oc + 0x200
    d[e + 0x400:e + 0x800] = ad                                  # 1. AD firmato Nintendo (la ACI di makerom -desc dlpchild:1 e' gia' quella giusta)

    if a.version is not None:                                    # 2. versione
        if not 0 <= a.version <= 0xFFFF:
            sys.exit("ERRORE: versione fuori da u16")
        struct.pack_into(">H", d, hb + 0x9C, a.version)
        struct.pack_into(">H", d, tb + 0xA6, a.version)
    struct.pack_into(">I", d, hb + 0x9C4, 0)                     # 3. content id
    d[oc + 0x18B] = 0x00
    d[oc + 0x18F] = 0x01
    exsize = struct.unpack_from("<I", d, oc + 0x180)[0]
    d[oc + 0x160:oc + 0x180] = hashlib.sha256(bytes(d[oc + 0x200:oc + 0x200 + exsize])).digest()   # 4. sul chiaro
    csize = struct.unpack_from(">Q", d, hb + 0x9C4 + 8)[0]
    plain = bytes(d[oc:oc + csize])
    key = bytes(16)
    ncch_crypt(d, oc, key, "cifro ")
    tmd_hashes(d, oc, hb)                                        # 5. sui byte cifrati
    open(a.cia, "wb").write(bytes(d))

    v = bytearray(open(a.cia, "rb").read())
    voc, vhb, vtb = parts(v)
    cr = vhb + 0x9C4
    ok1 = hashlib.sha256(bytes(v[voc:voc + csize])).digest() == bytes(v[cr + 0x10:cr + 0x30])
    back = bytearray(v)
    ncch_crypt(back, voc, key, "rileggo")
    ok2 = bytes(back[voc:voc + csize]) == plain
    ok3 = hashlib.sha256(bytes(back[voc + 0x200:voc + 0x200 + exsize])).digest() == bytes(v[voc + 0x160:voc + 0x180])
    print(f"  flags NCCH {v[voc+0x188:voc+0x190].hex()}  versione TMD {struct.unpack_from('>H', v, vhb+0x9C)[0]}  ticket {struct.unpack_from('>H', v, vtb+0xA6)[0]}")
    print(f"  hash TMD sul cifrato={ok1}  ciclo cifra/decifra={ok2}  hash exheader sul chiaro={ok3}")
    if not (ok1 and ok2 and ok3):
        sys.exit(1)


if __name__ == "__main__":
    main()
