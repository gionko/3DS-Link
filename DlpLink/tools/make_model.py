#!/usr/bin/env python3
"""make_model.py -- il MODELLO del figlio Download Play: il figlio finito senza la regione RomFS.

3DS Link e' UN CIA solo: il figlio (child.cia) non si distribuisce, lo costruisce l'app sulla console (AzaharLink/ConfigApp/dlpc/, contratto dlpc.h,
progetto in DlpLink/docs/COMPOSITORE.md). Da qui esce quello che all'app serve di gia' fatto: tutto il figlio tranne il RomFS (che porta l'app stessa e il
plugin), cioe' intestazione CIA, certificati, ticket, TMD, intestazione NCCH, exheader con il descrittore Nintendo, ExeFS dell'installatore e il meta con
l'icona. Circa 220 KB: l'app lo tiene come risorsa (child_model.bin -> simbolo child_model_bin).

Uso:
  make_model.py CHILD.cia MODELLO.bin [--emu]   CHILD.cia = il figlio FINITO (make_dlp_child.sh: out/3dslink-child.cia, cifrato a chiave zero) oppure la
                                                variante emulatore (out/3dslink-child.emu.cia, NoCrypto: solo per i test e per Azahar; --emu lo dichiara
                                                e il file deve esserlo davvero)
  make_model.py --check MODELLO.bin             controlla un modello con le stesse regole del compositore (esce con 1 se e' rifiutato)

Il formato (little endian). Intestazione di 0x80 byte, poi il corpo = i primi `head` byte del CIA (fino al posto del RomFS nell'NCCH) e i `meta` byte del meta.
  0x00 "DLPM"   0x04 u32 formato (1)   0x08 u32 dimensione intestazione (0x80)   0x0C u32 flag (bit 0: modello emulatore, NoCrypto)
  0x10 u16 versione del TMD che il modello porta   0x12 u16 0   0x14 u32 byte del corpo   0x18 u32 head   0x1C u32 meta
  0x20 u32 offset del TMD nel CIA   0x24 u32 byte del TMD   0x28 u32 offset del contenuto (NCCH) nel CIA   0x2C u32 offset del RomFS nell'NCCH (byte)
  0x30 u64 Title ID del figlio   0x38..0x5F zeri   0x60 SHA-256 di intestazione[0..0x60) + corpo
I campi che dipendono dal RomFS (dimensione del contenuto nell'intestazione CIA, record del TMD e le sue catene di hash, dimensione e hash del RomFS
nell'NCCH, dimensione dell'NCCH) sono AZZERATI: li scrive il compositore. Il modello NON dipende dal RomFS del figlio da cui nasce (quindi nessun uovo e gallina:
l'app tiene il modello, il figlio tiene l'app) ma non e' identico bit per bit da un figlio all'altro: makerom mette a caso la chiave e l'id del biglietto, e le tre
firme (biglietto, TMD, intestazione NCCH) cambiano a ogni esecuzione. Sono tutte firme che dopo finalize_child.py non tornano comunque (Luma le ignora). Provato
2026-10-21: due figli dallo stesso ELF con due RomFS diversi (uno vero, uno di 3 file a caso) danno modelli che differiscono solo in quei campi (817 byte su 250944).
Il figlio che il compositore ne tira fuori e' lo stesso byte per byte di quello di makerom da cui il modello e' stato fatto, con lo stesso carico.
"""
import argparse, hashlib, struct, sys

MAGIC = b"DLPM"
FORMAT = 1
HDR = 0x80
HASH_AT = 0x60
MAX_BODY = 0x400000                     # un installatore da piu' di 4 MiB non e' questo
SIG = {0x10000: (0x200, 0x3C), 0x10001: (0x100, 0x3C), 0x10002: (0x3C, 0x40),
       0x10003: (0x200, 0x3C), 0x10004: (0x100, 0x3C), 0x10005: (0x3C, 0x40)}
TID_HI = 0x00040001


def al(x, a=64):
    return (x + a - 1) & ~(a - 1)


def le16(d, o): return struct.unpack_from("<H", d, o)[0]
def le32(d, o): return struct.unpack_from("<I", d, o)[0]
def le64(d, o): return struct.unpack_from("<Q", d, o)[0]
def be16(d, o): return struct.unpack_from(">H", d, o)[0]
def be32(d, o): return struct.unpack_from(">I", d, o)[0]
def be64(d, o): return struct.unpack_from(">Q", d, o)[0]


def cia_layout(d):
    """Le posizioni nel CIA (come verify_cia.py): ritorna un dizionario o solleva ValueError."""
    if len(d) < 0x2040:
        raise ValueError("troppo corto per un CIA")
    hsz, typ, ver, certsz, tiksz, tmdsz, metasz = struct.unpack_from("<IHHIIII", d, 0)
    if hsz != 0x2020 or typ != 0 or ver != 0:
        raise ValueError("l'intestazione CIA non e' quella di un CIA normale")
    if not (0 < certsz <= 0x10000 and 0 < tiksz <= 0x10000 and 0 < tmdsz <= 0x10000):
        raise ValueError("dimensioni di certificati/ticket/TMD fuori misura")
    oc_ = al(hsz)
    ot = oc_ + al(certsz)
    om = ot + al(tiksz)
    oc = om + al(tmdsz)
    ms = be32(d, om) if om + 4 <= len(d) else 0
    if ms not in SIG:
        raise ValueError("tipo di firma del TMD sconosciuto")
    hb = om + 4 + sum(SIG[ms])
    ts = be32(d, ot)
    if ts not in SIG:
        raise ValueError("tipo di firma del ticket sconosciuto")
    tb = ot + 4 + sum(SIG[ts])
    if hb + 0x9C4 + 0x30 > om + tmdsz or tb + 0xB0 > ot + tiksz:
        raise ValueError("TMD o ticket piu' corti di quello che servono")
    return dict(hsz=hsz, certsz=certsz, tiksz=tiksz, tmdsz=tmdsz, metasz=metasz, ot=ot, om=om, oc=oc, hb=hb, tb=tb)


def parse_model(m):
    """Le regole del compositore (dlpc.c, model_parse): ritorna un dizionario o solleva ValueError con il motivo. Un modello troncato e' un rifiuto, non un'eccezione."""
    try:
        return _parse_model(m)
    except struct.error:
        raise ValueError("modello troncato") from None


def _parse_model(m):
    if len(m) < HDR or m[:4] != MAGIC:
        raise ValueError("non e' un modello (magic)")
    fmt, hsz, flags = struct.unpack_from("<III", m, 4)
    if fmt != FORMAT or hsz != HDR:
        raise ValueError("formato o intestazione sconosciuti")
    if flags & ~1:
        raise ValueError("flag sconosciuti")
    ver, rsv, body, head, meta, tmd_off, tmd_size, ncch_off, romfs_off = struct.unpack_from("<HHIIIIIII", m, 0x10)
    tid = le64(m, 0x30)
    if rsv != 0 or any(m[0x38:HASH_AT]):
        raise ValueError("campi riservati non a zero")
    if body > MAX_BODY or len(m) != HDR + body or head + meta != body:
        raise ValueError("dimensioni incoerenti")
    if hashlib.sha256(bytes(m[:HASH_AT]) + bytes(m[HDR:])).digest() != bytes(m[HASH_AT:HASH_AT + 32]):
        raise ValueError("somma di controllo sbagliata (risorsa danneggiata)")
    b = bytes(m[HDR:HDR + head])
    meta_b = bytes(m[HDR + head:])
    try:
        L = cia_layout(b)
    except ValueError as e:
        raise ValueError(f"CIA del modello: {e}") from None
    if L["oc"] != ncch_off or L["om"] != tmd_off or L["tmdsz"] != tmd_size or L["metasz"] != meta or meta != 0x3AC0:
        raise ValueError("offset dell'intestazione diversi dal CIA")
    if le64(b, 0x18) != 0:
        raise ValueError("la dimensione del contenuto non e' azzerata")
    if romfs_off < 0xA00 or romfs_off % 0x200 or head != ncch_off + romfs_off or head > body:
        raise ValueError("posto del RomFS fuori misura")
    if be16(b, L["hb"] + 0x9E) != 1:
        raise ValueError("il TMD deve avere un solo contenuto")
    if be16(b, L["hb"] + 0x9C) != ver or be16(b, L["tb"] + 0xA6) != ver:
        raise ValueError("versione del TMD / ticket diversa da quella dichiarata")
    if be64(b, L["hb"] + 0x4C) != tid or be64(b, L["tb"] + 0x9C) != tid or tid >> 32 != TID_HI:
        raise ValueError("Title ID incoerente (TMD, ticket, intestazione) o non di un figlio DLP")
    if be64(b, L["hb"] + 0x9C4 + 8) != 0 or be16(b, L["hb"] + 0x9C4 + 6) & 1:
        raise ValueError("record del contenuto non azzerato o cifrato a livello CIA")
    n = ncch_off
    if b[n + 0x100:n + 0x104] != b"NCCH" or le64(b, n + 0x108) != tid or le64(b, n + 0x118) != tid:
        raise ValueError("NCCH assente o con un Title ID diverso")
    if le32(b, n + 0x104) != 0 or le32(b, n + 0x1B4) != 0:
        raise ValueError("il modello non deve avere dimensione dell'NCCH ne' del RomFS (e' il figlio intero?)")
    if le32(b, n + 0x1B0) * 0x200 != romfs_off or not 1 <= le32(b, n + 0x1B8) <= 8:
        raise ValueError("l'NCCH non ha un posto per il RomFS che si possa riempire")
    ex_off, ex_size = le32(b, n + 0x1A0) * 0x200, le32(b, n + 0x1A4) * 0x200
    ehr = le32(b, n + 0x1A8)                                     # regione hash dell'ExeFS (unita' da 0x200): il compositore ne ricontrolla l'hash
    if le32(b, n + 0x180) != 0x400 or ex_off < 0xA00 or ex_off + ex_size > romfs_off or not 1 <= ehr <= 8 or ehr * 0x200 > ex_size:
        raise ValueError("exheader / ExeFS fuori posto")
    fl = b[n + 0x188:n + 0x190]
    emu = bool(flags & 1)
    if (emu and not (fl[7] & 4)) or (not emu and fl[7] != 0x01) or fl[5] != 0x03:
        raise ValueError("i flag dell'NCCH non sono quelli della variante dichiarata")
    if meta_b[0x400:0x404] != b"SMDH":
        raise ValueError("il meta non porta l'SMDH")
    return dict(version=ver, tid=tid, emu=emu, body=body, head=head, meta=meta, tmd_off=tmd_off, tmd_size=tmd_size, ncch_off=ncch_off, romfs_off=romfs_off,
                size=len(m))


def make(child):
    d = bytearray(child)
    L = cia_layout(d)
    oc, om, hb, tb = L["oc"], L["om"], L["hb"], L["tb"]
    if len(d) > 0x02000000:
        raise ValueError("il figlio supera 32 MiB")
    n = oc
    if d[n + 0x100:n + 0x104] != b"NCCH":
        raise ValueError("il contenuto 0 non e' un NCCH")
    nsz = be16(d, hb + 0x9E)
    if nsz != 1:
        raise ValueError("il figlio deve avere un solo contenuto")
    tid = be64(d, hb + 0x4C)
    ver = be16(d, hb + 0x9C)
    romfs_off = le32(d, n + 0x1B0) * 0x200
    romfs_size = le32(d, n + 0x1B4) * 0x200
    if romfs_size == 0 or romfs_off == 0:
        raise ValueError("il figlio non ha RomFS: niente da togliere")
    if oc + al(le64(d, 0x18)) + L["metasz"] != len(d) or le32(d, n + 0x104) * 0x200 != le64(d, 0x18):
        raise ValueError("il CIA non torna con la propria intestazione")
    if romfs_off + romfs_size != le32(d, n + 0x104) * 0x200:
        raise ValueError("il RomFS non e' l'ultima parte dell'NCCH")
    emu = bool(d[n + 0x18F] & 4)
    head = oc + romfs_off
    meta_off = oc + al(le64(d, 0x18))
    # azzera quello che dipende dal RomFS
    d[0x18:0x20] = bytes(8)                                      # dimensione del contenuto (CIA)
    cr, info = hb + 0x9C4, hb + 0xC4
    d[cr + 8:cr + 0x30] = bytes(0x28)                            # record: dimensione + SHA-256
    for k in range(64):
        first, count = struct.unpack_from(">HH", d, info + 0x24 * k)
        if count:
            d[info + 0x24 * k + 4:info + 0x24 * k + 0x24] = bytes(0x20)
    d[hb + 0xA4:hb + 0xC4] = bytes(0x20)                         # SHA-256 dei 64 info record
    d[n + 0x104:n + 0x108] = bytes(4)                            # dimensione dell'NCCH
    d[n + 0x1B4:n + 0x1B8] = bytes(4)                            # dimensione del RomFS
    d[n + 0x1E0:n + 0x200] = bytes(0x20)                         # hash della regione hash del RomFS
    body = bytes(d[:head]) + bytes(d[meta_off:meta_off + L["metasz"]])
    hdr = bytearray(HDR)
    hdr[0:4] = MAGIC
    struct.pack_into("<III", hdr, 4, FORMAT, HDR, 1 if emu else 0)
    struct.pack_into("<HHIIIIIII", hdr, 0x10, ver, 0, len(body), head, L["metasz"], om, L["tmdsz"], oc, romfs_off)
    struct.pack_into("<Q", hdr, 0x30, tid)
    hdr[HASH_AT:HASH_AT + 32] = hashlib.sha256(bytes(hdr[:HASH_AT]) + body).digest()
    return bytes(hdr) + body, emu


def main():
    ap = argparse.ArgumentParser(description="il modello del figlio Download Play (vedi la docstring)")
    ap.add_argument("child", nargs="?", help="il figlio finito (.cia)")
    ap.add_argument("model", nargs="?", help="il modello da scrivere (.bin)")
    ap.add_argument("--emu", action="store_true", help="il figlio e' la variante emulatore (NoCrypto); il file deve esserlo")
    ap.add_argument("--check", metavar="MODELLO", help="controlla un modello e esce")
    a = ap.parse_args()
    try:
        if a.check:
            info = parse_model(open(a.check, "rb").read())
            print(f"modello valido: {'emulatore' if info['emu'] else 'vero'}, versione TMD {info['version']}, Title ID {info['tid']:016X}, {info['size']} byte, "
                  f"head {info['head']} + meta {info['meta']}, RomFS a NCCH+0x{info['romfs_off']:X}")
            return
        if not a.child or not a.model:
            ap.error("servono CHILD.cia e MODELLO.bin (oppure --check)")
        model, emu = make(open(a.child, "rb").read())
        if emu != a.emu:
            raise ValueError(f"il file {'e' if emu else 'non e'}' la variante emulatore (NoCrypto) ma {'non ' if emu else ''}e' stato chiesto --emu")
        info = parse_model(model)                                # il modello scritto passa le regole del compositore
        open(a.model, "wb").write(model)
        print(f"{a.model}: modello {'emulatore' if emu else 'vero'}, {len(model)} byte, versione TMD {info['version']} ({info['version'] >> 10}.{(info['version'] >> 4) & 63}.{info['version'] & 15}), "
              f"Title ID {info['tid']:016X}, sha256 {hashlib.sha256(model).hexdigest()[:16]}")
    except (ValueError, OSError, struct.error) as e:             # (un figlio troncato e' un errore con un motivo, non una traceback)
        sys.exit(f"ERRORE: {e}")


if __name__ == "__main__":
    main()
