#!/usr/bin/env python3
"""verify_cia.py -- verificatore INDIPENDENTE di CIA (figlio DLP e CIA di 3DS Link).

Non importa nulla da finalize_child.py: rilegge tutto dai byte. Solo libreria standard + AES-128-CTR (modulo
`cryptography`, oppure `Crypto` di pycryptodome, oppure il comando `openssl`). Esce con codice 1 se un controllo fallisce.

Uso:
  verify_cia.py FILE.cia --kind child [--tid 000400010F83D100] [--version 1024] [--romfs-dir DIR] [--max-cia 33554432]
                         [--expect-strings NAME DESC AUTHOR] [--elf child.elf]
  verify_cia.py FILE.cia --kind app   [--tid 000400000F83D100] [--version 1024] [--elf 3DSLink.elf]
  verify_cia.py FILE.cia --kind child --emu     la variante per Azahar (NoCrypto, senza Access Descriptor di Nintendo): niente controlli di
                                                cifratura / AD, il resto uguale

Controlli (ognuno stampa OK / FAIL / INFO):
  CIA     dimensioni dell'intestazione, allineamenti, lunghezza del file, bitmap dei content
  TMD     catena di hash: contenuto -> chunk record -> info record -> header; flag di cifratura CIA a 0
  ID      Title ID uguale in ticket, TMD, NCCH (program id e partition id); versione TMD == ticket (== --version)
  NCCH    magic, dimensione == TMD, flag, hash dell'exheader / ExeFS / RomFS sul CHIARO (decifra a chiave zero se serve)
  EXEFS   tabella file, hash di ogni file, .code/icon/logo presenti, icon = SMDH
  RomFS   IVFC: master hash -> L1 -> L2 -> L3 (blocchi da 4 KB, ultimo riempito di zeri), albero di directory, tabelle di
          hash dei nomi, estrazione di ogni file ed eventuale confronto byte per byte con --romfs-dir
  META    SMDH presente; cio' che richiede rawhost (DlpTest/rawhost/source/main.c parse_child)
  SMDH    icona ExeFS == SMDH del meta; nessun angolo dell'icona 48x48 e' nero puro (smdhtool appiattisce l'alfa sul NERO: un'icona con angoli
          trasparenti li mostra neri); regione libera; con --expect-strings: nome (descrizione corta, <= 63 caratteri), descrizione (lunga,
          <= 127) e autore uguali a quelli attesi in OGNI lingua (quello che il Download Play mostra)
  CHILD   Access Descriptor jolly 00040001FFFFFFFF, SCI senza bit "SD application", flags[7]==0x01, flags[5]==0x03
"""
import argparse, hashlib, json, os, struct, subprocess, sys

R = []   # risultati (stato, gruppo, messaggio)


def rec(state, group, msg):
    R.append((state, group, msg))
    print(f"[{state:4}] {group:6} {msg}")


def check(cond, group, msg):
    rec("OK" if cond else "FAIL", group, msg)
    return bool(cond)


def al(x, a=64):
    return (x + a - 1) & ~(a - 1)


def be16(d, o): return struct.unpack_from(">H", d, o)[0]
def be32(d, o): return struct.unpack_from(">I", d, o)[0]
def be64(d, o): return struct.unpack_from(">Q", d, o)[0]
def le16(d, o): return struct.unpack_from("<H", d, o)[0]
def le32(d, o): return struct.unpack_from("<I", d, o)[0]
def le64(d, o): return struct.unpack_from("<Q", d, o)[0]
def sha(b): return hashlib.sha256(bytes(b)).digest()


# ------------------------------------------------------------------ AES-128-CTR
def aes_ctr(key, iv, data):
    try:
        from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
        c = Cipher(algorithms.AES(key), modes.CTR(iv)).encryptor()
        return c.update(bytes(data)) + c.finalize()
    except ImportError:
        pass
    try:
        from Crypto.Cipher import AES
        from Crypto.Util import Counter
        ctr = Counter.new(128, initial_value=int.from_bytes(iv, "big"))
        return AES.new(key, AES.MODE_CTR, counter=ctr).encrypt(bytes(data))
    except ImportError:
        pass
    r = subprocess.run(["openssl", "enc", "-aes-128-ctr", "-nopad", "-K", key.hex(), "-iv", iv.hex()],
                       input=bytes(data), stdout=subprocess.PIPE, check=True)
    return r.stdout


SIG = {0x10000: (0x200, 0x3C), 0x10001: (0x100, 0x3C), 0x10002: (0x3C, 0x40),
       0x10003: (0x200, 0x3C), 0x10004: (0x100, 0x3C), 0x10005: (0x3C, 0x40)}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cia")
    ap.add_argument("--kind", choices=("child", "app"), required=True)
    ap.add_argument("--tid")
    ap.add_argument("--version", type=int)
    ap.add_argument("--romfs-dir", help="cartella: ogni file li' dentro deve ritrovarsi identico nel RomFS (e viceversa)")
    ap.add_argument("--max-cia", type=int, default=0x02000000, help="limite del campo 'required' del beacon DLP (3dbrew: <= 0x02000000)")
    ap.add_argument("--elf", help="ELF dell'eseguibile: ogni `svc N` usato dal codice deve essere nella maschera SVC dell'exheader")
    ap.add_argument("--json", help="scrive il riepilogo in JSON")
    ap.add_argument("--expect-strings", nargs=3, metavar=("NAME", "DESC", "AUTHOR"), help="le tre stringhe dello SMDH (descrizione corta, lunga, autore) in tutte le lingue")
    ap.add_argument("--expect-banner", action="store_true", help="l'ExeFS porta il banner della HOME (CBMD con il modello CGFX e la musica CWAV)")
    ap.add_argument("--emu", action="store_true", help="figlio per Azahar: NoCrypto e Access Descriptor di makerom (niente controlli del figlio vero)")
    a = ap.parse_args()
    d = open(a.cia, "rb").read()
    G = "CIA"

    # ---------------------------------------------------------------- contenitore CIA
    hsz, typ, ver, certsz, tiksz, tmdsz, metasz = struct.unpack_from("<IHHIIII", d, 0)
    csz = le64(d, 0x18)
    check(hsz == 0x2020 and typ == 0 and ver == 0, G, f"header 0x{hsz:X} tipo {typ} versione {ver}")
    oc_ = al(hsz); ot = oc_ + al(certsz); om = ot + al(tiksz); oc = om + al(tmdsz); omt = oc + al(csz)
    end = omt + metasz if metasz else oc + csz
    check(len(d) == end, G, f"lunghezza file {len(d)} == fine attesa {end} (cert 0x{certsz:X} tik 0x{tiksz:X} tmd 0x{tmdsz:X} content 0x{csz:X} meta 0x{metasz:X})")
    check(certsz == 0xA00 and tiksz == 0x350, G, "catena certificati 0xA00 e ticket 0x350")
    check(b"Root-CA00000003-XS0000000c" in d[oc_:oc_ + certsz] or b"XS0000000c" in d[oc_:oc_ + certsz], G, "certificato XS0000000c presente")
    check(bytes(d[oc_:oc_ + certsz]).count(b"Root") >= 3, G, "catena con almeno 3 certificati (Root, CA, XS/CP)")

    # ---------------------------------------------------------------- TMD / ticket
    G = "TMD"
    ms = be32(d, om)
    check(ms in SIG, G, f"tipo firma TMD 0x{ms:X}")
    hb = om + 4 + sum(SIG[ms])
    tmd_end = om + tmdsz
    n = be16(d, hb + 0x9E)
    tid = be64(d, hb + 0x4C)
    tmdver = be16(d, hb + 0x9C)
    cr, info = hb + 0x9C4, hb + 0xC4
    check(cr + 0x30 * n <= tmd_end, G, f"{n} content record dentro il TMD")
    pos = oc; chunk_ok = True; sizes = []
    for i in range(n):
        cid, cidx, ctype, csize_i = struct.unpack_from(">IHHQ", d, cr + 0x30 * i)
        h = bytes(d[cr + 0x30 * i + 0x10:cr + 0x30 * i + 0x30])
        good = sha(d[pos:pos + csize_i]) == h
        chunk_ok &= good
        check(good, G, f"content {i}: id {cid:08X} idx {cidx} tipo 0x{ctype:04X} size 0x{csize_i:X} SHA-256 sui byte memorizzati")
        check(not (ctype & 1), G, f"content {i}: flag 'cifrato CIA' a 0 (il nostro contenuto e' cifrato SOLO a livello NCCH)")
        sizes.append(csize_i); pos += al(csize_i)
    check(sum(al(s) for s in sizes) == al(csz) or sum(al(s) for s in sizes) == csz, G, "somma dei content (allineati) == ContentSize dell'header CIA")
    ir_ok = True
    for k in range(64):
        f_, c_ = struct.unpack_from(">HH", d, info + 0x24 * k)
        if c_:
            ir_ok &= sha(d[cr + 0x30 * f_:cr + 0x30 * (f_ + c_)]) == bytes(d[info + 0x24 * k + 4:info + 0x24 * k + 0x24])
    check(ir_ok, G, "info record: SHA-256 dei chunk record coperti")
    check(sha(d[info:info + 0x24 * 64]) == bytes(d[hb + 0xA4:hb + 0xC4]), G, "header TMD: SHA-256 dei 64 info record")

    ts = be32(d, ot)
    tb = ot + 4 + sum(SIG[ts])
    tik_tid, tik_ver = be64(d, tb + 0x9C), be16(d, tb + 0xA6)
    G = "ID"
    check(tid == tik_tid, G, f"Title ID TMD {tid:016X} == ticket {tik_tid:016X}")
    check(tmdver == tik_ver, G, f"versione TMD {tmdver} (0x{tmdver:04X} = {tmdver>>10}.{(tmdver>>4)&63}.{tmdver&15}) == ticket {tik_ver}")
    if a.tid: check(tid == int(a.tid, 16), G, f"Title ID atteso {a.tid}")
    if a.version is not None: check(tmdver == a.version, G, f"versione attesa {a.version}")
    want_hi = 0x00040001 if a.kind == "child" else 0x00040000
    check(tid >> 32 == want_hi, G, f"TID-high {tid>>32:08X} == {want_hi:08X} ({'figlio DLP' if a.kind=='child' else 'applicazione su SD'})")
    if a.kind == "child":
        rec("INFO", G, f"byte basso del TID ({tid & 0xFF:#04x}): 3dbrew dice 'low 4 bits del TID-low a zero', ma il figlio reale di Zelda TFH ha 0x02 (cattura): non vincolante")

    # ---------------------------------------------------------------- NCCH
    G = "NCCH"
    n0 = bytearray(d[oc:oc + sizes[0]])
    check(n0[0x100:0x104] == b"NCCH", G, "magic NCCH")
    check(le32(n0, 0x104) * 0x200 == sizes[0], G, f"dimensione NCCH {le32(n0, 0x104)*0x200:#x} == content TMD {sizes[0]:#x}")
    pid_le = bytes(n0[0x108:0x110])
    check(le64(n0, 0x108) == tid and le64(n0, 0x118) == tid, G, "partition id e program id == Title ID")
    fl = bytes(n0[0x188:0x190])
    f7 = fl[7]
    rec("INFO", G, f"flags {fl.hex()}  (flags[3]={fl[3]:#x} flags[5]={fl[5]:#x} flags[7]={f7:#x}; 1=FixedCryptoKey 2=NoMountRomFS 4=NoCrypto)")
    exsz = le32(n0, 0x180)
    exoff = 0x200
    fx_off, fx_sz, fx_hr = le32(n0, 0x1A0) * 0x200, le32(n0, 0x1A4) * 0x200, le32(n0, 0x1A8) * 0x200
    rf_off, rf_sz, rf_hr = le32(n0, 0x1B0) * 0x200, le32(n0, 0x1B4) * 0x200, le32(n0, 0x1B8) * 0x200
    secs = [("exheader", exoff, 0x800, 1), ("ExeFS", fx_off, fx_sz, 2), ("RomFS", rf_off, rf_sz, 3)]
    plainok = True
    if f7 & 4:
        rec("INFO", G, "NoCrypto: contenuto in chiaro")
    elif f7 & 1 and (tid >> 32) & 0x10 == 0:
        key = bytes(16)
        for name, off, sz, sec in secs:
            if sz:
                n0[off:off + sz] = aes_ctr(key, bytes(reversed(pid_le)) + bytes([sec]) + b"\0" * 7, n0[off:off + sz])
        rec("INFO", G, "FixedCryptoKey a chiave di soli zeri: exheader/ExeFS/RomFS decifrati (CTR: partitionId BE | sezione | zeri)")
    else:
        rec("FAIL", G, "cifratura NCCH standard o chiave fissa di sistema: non verificabile qui (serve KeyX)")
        plainok = False
    if plainok:
        check(sha(n0[exoff:exoff + exsz]) == bytes(n0[0x160:0x180]), G, f"hash exheader (primi 0x{exsz:X} byte, sul chiaro)")
        if fx_sz: check(sha(n0[fx_off:fx_off + fx_hr]) == bytes(n0[0x1C0:0x1E0]), G, f"hash ExeFS (primi 0x{fx_hr:X} byte, sul chiaro)")
        if rf_sz: check(sha(n0[rf_off:rf_off + rf_hr]) == bytes(n0[0x1E0:0x200]), G, f"hash RomFS (primi 0x{rf_hr:X} byte, sul chiaro)")

    # ---------------------------------------------------------------- CHILD: exheader / AD
    if a.kind == "child" and a.emu:
        G = "CHILD"
        check(bool(f7 & 4), G, f"flags[7]={f7:#x}: NoCrypto acceso (Azahar rifiuta un NCCH cifrato: 'Blocked unauthorized encrypted CIA installation')")
        check(fl[5] == 0x03 and rf_sz > 0, G, "flags[5] == 0x03 (Data+Executable) con RomFS")
        if plainok:
            ex = n0[exoff:exoff + 0x800]
            check(le64(ex, 0x200) == tid, G, "ACI programId == Title ID")
            check(not (ex[0xD] & 2), G, f"SCI flags {ex[0xD]:#x}: niente 'SD application' (i figli DLP stanno in NAND)")
    elif a.kind == "child":
        G = "CHILD"
        check(f7 == 0x01, G, "flags[7] == 0x01 (solo FixedCryptoKey)")
        check(fl[5] == 0x03 and rf_sz > 0, G, "flags[5] == 0x03 (Data+Executable) con RomFS")
        if plainok:
            ex = n0[exoff:exoff + 0x800]
            ad_pid = le64(ex, 0x400 + 0x200)
            check(ad_pid == 0x00040001FFFFFFFF, G, f"Access Descriptor programId {ad_pid:016X} (jolly)")
            check(le64(ex, 0x200) == tid, G, "ACI programId == Title ID")
            check(not (ex[0xD] & 2), G, f"SCI flags {ex[0xD]:#x}: niente 'SD application' (i figli DLP stanno in NAND)")
    else:
        G = "APP"
        if plainok:
            ex = n0[exoff:exoff + 0x800]
            check(bool(ex[0xD] & 2), G, f"SCI flags {ex[0xD]:#x}: bit 'SD application' acceso (app su SD)")
            check(le64(ex, 0x200) == tid, G, "ACI programId == Title ID")
            check(le64(ex, 0x400 + 0x200) == tid, G, "Access Descriptor programId == Title ID")

    if plainok:
        # capability kernel dell'ACI: exheader+0x370, 28 parole. Descrittore SVC: 0b11110 | indice(3 bit) | maschera(24 bit)
        ex = n0[exoff:exoff + 0x800]
        allowed = set()
        for k in range(28):
            w = le32(ex, 0x370 + 4 * k)
            if w >> 27 == 0b11110:
                allowed |= {((w >> 24) & 7) * 24 + b for b in range(24) if (w >> b) & 1}
        rec("INFO", "SVC", f"maschera SVC dell'exheader: {len(allowed)} chiamate permesse")
        if a.elf:
            objdump = os.environ.get("OBJDUMP") or next((c for c in ("arm-none-eabi-objdump", "/opt/devkitpro/devkitARM/bin/arm-none-eabi-objdump")
                                                         if subprocess.run(["sh", "-c", f"command -v {c}"], capture_output=True).returncode == 0), None)
            if objdump is None:
                rec("INFO", "SVC", "objdump non trovato: controllo --elf saltato (eseguirlo dentro Docker devkitarm)")
            else:
                import re
                pr = subprocess.run([objdump, "-d", a.elf], capture_output=True, text=True); txt = pr.stdout
                if pr.returncode: rec("FAIL", "SVC", f"objdump ha fallito: {pr.stderr[:120]}")
                used = {int(m.group(1), 16) for m in re.finditer(r"svc\s+(0x[0-9a-fA-F]+)", txt)}
                missing = sorted(used - allowed)
                check(not missing, "SVC", f"{len(used)} SVC usati da {os.path.basename(a.elf)}; fuori maschera: {[hex(x) for x in missing] or 'nessuno'}")
    # ---------------------------------------------------------------- ExeFS
    G = "EXEFS"
    exefs = {}
    if plainok and fx_sz:
        eh = n0[fx_off:fx_off + 0x200]
        for i in range(10):
            nm = bytes(eh[16 * i:16 * i + 8]).rstrip(b"\0").decode("ascii", "replace")
            if not nm.isprintable(): nm = repr(bytes(eh[16 * i:16 * i + 8]))
            o, s = le32(eh, 16 * i + 8), le32(eh, 16 * i + 12)
            if not nm: continue
            blob = n0[fx_off + 0x200 + o:fx_off + 0x200 + o + s]
            h = bytes(eh[0x200 - 0x20 * (i + 1):0x200 - 0x20 * i])
            check(len(blob) == s and sha(blob) == h, G, f"{nm:6} off 0x{o:X} size 0x{s:X} hash")
            exefs[nm] = bytes(blob)
        check(".code" in exefs and "icon" in exefs, G, "ExeFS contiene .code e icon")
        if a.kind == "child":
            check("logo" in exefs, G, "ExeFS contiene logo (makerom -exefslogo)")
        check(exefs.get("icon", b"")[:4] == b"SMDH", G, "icon = SMDH")
        if a.expect_banner:
            bn = exefs.get("banner", b"")
            # CBMD: l'intestazione del banner (0x88 byte: versione, offset del CGFX comune e dei CGFX di regione, offset del CWAV), poi i CGFX (il modello LZ11) e il CWAV (la musica)
            cwav = le32(bn, 0x84) if len(bn) >= 0x88 else 0
            cg = le32(bn, 8) if len(bn) >= 0x88 else 0
            check(bn[:4] == b"CBMD" and len(bn) >= 0x20000 and 0x88 <= cg < len(bn) and 0 < cwav < len(bn) and bn[cwav:cwav + 4] == b"CWAV", G,
                  f"banner = CBMD con il modello CGFX e la musica CWAV ({len(bn)} byte, CGFX a 0x{cg:X}, CWAV a 0x{cwav:X})")
    # ---------------------------------------------------------------- META
    G = "META"
    if metasz:
        meta = d[omt:omt + metasz]
        check(metasz == 0x3AC0 and meta[0x400:0x404] == b"SMDH", G, f"meta 0x{metasz:X}, SMDH a +0x400")
        check(omt + 0x400 + 0x24C0 + 0x1200 <= len(d), G, "rawhost parse_child: offMeta+0x400+0x24C0+0x1200 <= dimensione CIA")
        transfer, required = omt, len(d)
        rec("INFO", G, f"beacon DLP: transfer_size (frame+0x38) = {transfer} (0x{transfer:X}); required (frame+0x3C) = {required} (0x{required:X})")
        if a.kind == "child":
            check(required <= a.max_cia, G, f"required {required} <= limite 3dbrew 0x{a.max_cia:X} ({a.max_cia} byte)")
            blocks = (transfer + 262080 - 1) // 262080
            rec("INFO", G, f"blocchi da 182x1440 = 262080 byte: {blocks}; frammenti {(transfer + 1439)//1440}; "
                           f"~{transfer/143700:.0f} s a 140 KiB/s (media misurata), ~{blocks*0.87:.0f} s senza rinvii")
    else:
        rec("INFO", G, "nessuna sezione meta")

    # ---------------------------------------------------------------- SMDH: cio' che l'utente vede (HOME, Download Play)
    G = "SMDH"
    if metasz and d[omt + 0x400:omt + 0x404] == b"SMDH":
        smdh = bytes(d[omt + 0x400:omt + 0x400 + 0x36C0])
        if exefs.get("icon"):
            check(exefs["icon"][:0x36C0] == smdh, G, "l'icona dell'ExeFS e' l'SMDH del meta (byte per byte)")
        check(le32(smdh, 0x2018) == 0xFFFFFFFF, G, f"blocco di regione {le32(smdh, 0x2018):#010x} (libero)")

        def pixel(x, y):                                     # icona grande: 48x48 RGB565 a piastrelle 8x8 in ordine di Morton, a smdh+0x24C0
            m = 0
            for i in range(3):
                m |= ((x >> i) & 1) << (2 * i) | ((y >> i) & 1) << (2 * i + 1)
            v = le16(smdh, 0x24C0 + ((y // 8) * 6 + x // 8) * 128 + m * 2)
            return ((v >> 11) & 31) << 3, ((v >> 5) & 63) << 2, (v & 31) << 3
        corners = {(x, y): pixel(x, y) for x in (0, 47) for y in (0, 47)}
        check(all(c != (0, 0, 0) for c in corners.values()), G,
              f"angoli dell'icona 48x48 non neri: {corners}  (nero = alfa appiattito da smdhtool: l'icona ha ancora gli angoli trasparenti)")

        def text(off, size):
            return smdh[off:off + size].decode("utf-16le").split("\0")[0]
        langs = {}
        for k in range(16):
            o = 8 + 0x200 * k
            langs[k] = (text(o, 0x80), text(o + 0x80, 0x100), text(o + 0x180, 0x80))
        if a.expect_strings:
            want = tuple(a.expect_strings)
            bad_l = [k for k, v in langs.items() if v != want]
            check(not bad_l, G, f"nome / descrizione / autore {want} in tutte le 16 lingue (diverse: {bad_l or 'nessuna'})")
            check(len(want[0]) <= 63 and len(want[1]) <= 127, G, f"nome {len(want[0])} <= 63 e descrizione {len(want[1])} <= 127 caratteri (limiti del beacon DLP)")
        else:
            rec("INFO", G, f"lingua 0: {langs[0]}")
    elif metasz:
        rec("FAIL", G, "il meta non contiene l'SMDH a +0x400")

    # ---------------------------------------------------------------- RomFS
    G = "ROMFS"
    files = {}
    if plainok and rf_sz:
        rf = bytes(n0[rf_off:rf_off + rf_sz])
        check(rf[:4] == b"IVFC" and le32(rf, 4) == 0x10000, G, "IVFC magic e id 0x10000")
        mh = le32(rf, 8)
        lv = []
        for k in range(3):
            o = 0xC + 0x18 * k
            lv.append((le64(rf, o), le64(rf, o + 8), 1 << le32(rf, o + 16)))
        (o1, s1, b1), (o2, s2, b2), (o3, s3, b3) = lv
        base3 = al(0x60 + mh, b3)
        phys3 = base3
        phys1 = phys3 + al(s3, b3)
        phys2 = phys1 + al(s1, b1)
        check(phys2 + al(s2, b2) == rf_sz, G, f"layout fisico header|L3|L1|L2 = 0x{phys2 + al(s2, b2):X} == dimensione RomFS 0x{rf_sz:X}")
        L1, L2, L3 = rf[phys1:phys1 + s1], rf[phys2:phys2 + s2], rf[phys3:phys3 + s3]

        def lvl_hash(data, bs):
            out = b""
            for p in range(0, len(data), bs):
                blk = data[p:p + bs]
                out += sha(blk + b"\0" * (bs - len(blk)))
            return out
        check(lvl_hash(L1, b1) == rf[0x60:0x60 + mh], G, f"master hash ({mh} byte) = SHA-256 dei blocchi di L1")
        check(lvl_hash(L2, b2) == L1[:s1] and s1 == 32 * ((s2 + b2 - 1) // b2), G, f"L1 ({s1} byte) = SHA-256 dei blocchi di L2")
        check(lvl_hash(L3, b3) == L2[:s2] and s2 == 32 * ((s3 + b3 - 1) // b3), G, f"L2 ({s2} byte) = SHA-256 dei blocchi di L3 ({(s3 + b3 - 1) // b3} blocchi da {b3})")
        # albero dei file (livello 3)
        hl = le32(L3, 0)
        dh_o, dh_s, dm_o, dm_s, fh_o, fh_s, fm_o, fm_s, fd_o = struct.unpack_from("<9I", L3, 4)
        check(hl == 0x28 and fd_o + 0 <= s3, G, f"RomFS L3: header 0x{hl:X}, dati a 0x{fd_o:X}, L3 = {s3} byte")

        def rhash(parent, name, buckets):
            h = parent ^ 123456789
            for i in range(0, len(name), 2):
                ch = name[i] | (name[i + 1] << 8)
                h = ((h >> 5) | (h << 27)) & 0xFFFFFFFF
                h ^= ch
            return h % buckets

        def nm(o, base):
            ln = le32(L3, o)
            return bytes(L3[o + 4:o + 4 + ln])
        dtab, ftab = dm_o, fm_o
        dh = [le32(L3, dh_o + 4 * i) for i in range(dh_s // 4)]
        fh = [le32(L3, fh_o + 4 * i) for i in range(fh_s // 4)]

        budget = [100000]                                   # una RomFS corrotta puo' avere puntatori ciclici

        def walk(doff, path):
            # directory entry: parent, sibling, child dir, child file, nextHash, nameLen, name
            parent, sib, cd, cf, nh, ln = struct.unpack_from("<6I", L3, dtab + doff)
            fo = cf
            while fo != 0xFFFFFFFF:
                budget[0] -= 1
                if budget[0] < 0 or ftab + fo + 0x20 > len(L3): raise ValueError("catena dei file corrotta")
                fp, fsib, fdata, fsize, fnh, fln = struct.unpack_from("<IIQQII", L3, ftab + fo)
                name = bytes(L3[ftab + fo + 0x20:ftab + fo + 0x20 + fln])
                # coerenza della tabella di hash dei nomi: la catena del bucket deve contenere questa entry
                b = fh[rhash(doff, name, len(fh))]; seen = False; hops = 0
                while b != 0xFFFFFFFF and hops < 100000:
                    if b == fo: seen = True; break
                    b = le32(L3, ftab + b + 0x18); hops += 1
                pth = path + "/" + name.decode("utf-16le")
                files[pth] = (bytes(L3[fd_o + fdata:fd_o + fdata + fsize]), seen, fd_o + fdata + fsize <= s3)
                fo = fsib
            do = cd
            while do != 0xFFFFFFFF:
                budget[0] -= 1
                if budget[0] < 0 or dtab + do + 0x18 > len(L3): raise ValueError("catena delle directory corrotta")
                _, dsib, _, _, _, dln = struct.unpack_from("<6I", L3, dtab + do)
                walk(do, path + "/" + bytes(L3[dtab + do + 0x18:dtab + do + 0x18 + dln]).decode("utf-16le"))
                do = dsib
        try:
            walk(0, "")
        except Exception as e:                                # noqa: BLE001 -- qualunque errore di struttura = FAIL, non crash
            rec("FAIL", G, f"albero RomFS illeggibile: {e}")
        check(all(v[1] for v in files.values()), G, "ogni file e' raggiungibile dalla tabella di hash dei nomi (come fa fs)")
        check(all(v[2] for v in files.values()), G, "ogni file sta dentro L3")
        for p, v in sorted(files.items()):
            rec("INFO", G, f"file {p}  {len(v[0])} byte  sha256 {sha(v[0]).hex()[:16]}")
        if a.romfs_dir:
            exp = {}
            for r, _, fs_ in os.walk(a.romfs_dir):
                for f in fs_:
                    full = os.path.join(r, f)
                    exp["/" + os.path.relpath(full, a.romfs_dir).replace(os.sep, "/")] = open(full, "rb").read()
            check(set(exp) == set(files), G, f"stesso insieme di file di {a.romfs_dir} ({len(exp)} file)")
            for p, b in sorted(exp.items()):
                check(p in files and files[p][0] == b, G, f"{p} identico all'originale ({len(b)} byte)")

    bad = [r for r in R if r[0] == "FAIL"]
    print(f"\nRIEPILOGO: {sum(r[0]=='OK' for r in R)} OK, {len(bad)} FAIL, {sum(r[0]=='INFO' for r in R)} INFO  --  {'VALIDO' if not bad else 'NON VALIDO'}")
    if a.json:
        json.dump({"file": a.cia, "tid": f"{tid:016X}", "version": tmdver, "size": len(d), "ok": not bad,
                   "results": R}, open(a.json, "w"), indent=1)
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
