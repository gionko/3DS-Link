#!/usr/bin/env python3
"""version.py -- schema di versione unico per la CIA di 3DS Link e per il figlio DLP.
  show                    stampa MAJOR.MINOR.MICRO letto da AzaharLink/VERSION (l'UNICO numero di 3DS Link: app, plugin, CIA, figlio, documenti)
  u16                     versione TMD u16 = (major<<10)|(minor<<4)|micro   (major 0-63, minor 0-63, micro 0-15)
  bump micro|minor|major  incrementa la versione e la scrive in TUTTI i punti (AzaharLink/Tests/set_version.py: VERSION, plugin, app, plgInfo, documenti)
  patch FILE.cia          scrive la versione di VERSION in TMD e ticket (makerom -ver perde il campo "minor")
  guard APP.cia MODEL.bin PLUGIN.3gx TSV [--check-only]
                          il registro di cio' che e' stato CONSEGNATO. 3DS Link e' un CIA solo: il figlio Download Play non e' piu' un file che si consegna, lo
                          fa l'app sulla console da tre cose (il CIA dell'app, il plugin e il modello del figlio che l'app porta), e un client Download Play di
                          sistema puo' aver tenuto un figlio con quel Title ID e quella versione (README DlpTest voce 14: stessa versione e contenuto diverso =
                          il client rilancia la copia VECCHIA). Quindi quello che una versione significa e' proprio quei tre file: il guard prende lo SHA-256 di
                          ciascuno, li unisce in un digest solo e blocca una versione gia' consegnata con un digest diverso. Registra una versione solo senza
                          --check-only (una riga di DlpLink/releases.tsv: versione TMD, X.Y.Z, digest, data e le prime cifre dei tre hash), perche' la registrazione
                          si fa quando una build si CONSEGNA a una console (build_all.sh --register), non a ogni build di prova.
                          (releases.tsv lo crea la prima registrazione: prima della prima consegna non esiste.)
"""
import datetime, hashlib, os, struct, sys

HERE = os.path.dirname(os.path.abspath(__file__))
VF = os.environ.get("VERSION_FILE", os.path.join(HERE, "..", "..", "AzaharLink", "VERSION"))
SET_VERSION = os.path.join(HERE, "..", "..", "AzaharLink", "Tests", "set_version.py")


def read():
    M, m, u = (int(x) for x in open(VF).read().strip().split("."))
    if not (0 <= M < 64 and 0 <= m < 64 and 0 <= u < 16):
        sys.exit("VERSION fuori range (max 63.63.15)")
    return M, m, u


def u16():
    M, m, u = read()
    return (M << 10) | (m << 4) | u


def al(x, a=64):
    return (x + a - 1) & ~(a - 1)


SIG = {0x10000: 0x240, 0x10001: 0x140, 0x10002: 0x80, 0x10003: 0x240, 0x10004: 0x140, 0x10005: 0x80}


def layout(d):
    """(offset corpo ticket, offset corpo TMD, offset contenuto 0)"""
    hsz, _, _, cs, ts, ms, mt = struct.unpack_from("<IHHIIII", d, 0)
    ot = al(hsz) + al(cs)
    om = ot + al(ts)
    oc = om + al(ms)
    return ot + SIG[struct.unpack_from(">I", d, ot)[0]], om + SIG[struct.unpack_from(">I", d, om)[0]], oc


cmd = sys.argv[1] if len(sys.argv) > 1 else "show"
if cmd == "show":
    print("%d.%d.%d" % read())
elif cmd == "u16":
    print(u16())
elif cmd == "bump":
    M, m, u = read()
    M, m, u = {"micro": (M, m, u + 1), "minor": (M, m + 1, 0), "major": (M + 1, 0, 0)}[sys.argv[2]]
    if os.environ.get("VERSION_FILE"):                # (a test with another file: only that file)
        open(VF, "w").write("%d.%d.%d\n" % (M, m, u))
        read()
        print("%d.%d.%d" % (M, m, u))
    else:                                             # the real one: every point of the sources changes together
        import subprocess
        sys.exit(subprocess.call([sys.executable, SET_VERSION, "%d.%d.%d" % (M, m, u)]))
elif cmd == "patch":
    d = bytearray(open(sys.argv[2], "rb").read())
    tb, hb, oc = layout(d)
    v = u16()
    struct.pack_into(">H", d, hb + 0x9C, v)
    struct.pack_into(">H", d, tb + 0xA6, v)
    open(sys.argv[2], "wb").write(bytes(d))
    print("versione %d.%d.%d = %d scritta in TMD e ticket" % (*read(), v))
elif cmd == "guard":
    args = [a for a in sys.argv[2:] if not a.startswith("--")]
    if len(args) != 4:
        sys.exit("uso: version.py guard APP.cia MODEL.bin PLUGIN.3gx TSV [--check-only]")
    app, model, plugin, tsv = args
    d = open(app, "rb").read()
    tb, hb, oc = layout(d)
    v = struct.unpack_from(">H", d, hb + 0x9C)[0]
    parts = [("app", hashlib.sha256(d).hexdigest()), ("model", hashlib.sha256(open(model, "rb").read()).hexdigest()),
             ("plugin", hashlib.sha256(open(plugin, "rb").read()).hexdigest())]
    digest = hashlib.sha256("".join("%s %s\n" % p for p in parts).encode()).hexdigest()
    brief = " ".join("%s=%s" % (n, h[:12]) for n, h in parts)
    rows = [l.rstrip("\n").split("\t") for l in open(tsv)] if os.path.exists(tsv) else []
    same = [r for r in rows if len(r) >= 3 and r[0] == str(v)]   # (a short or blank row is ignored, not an IndexError)
    if any(r[2] != digest for r in same):
        sys.exit("ERRORE: la versione %d e' gia' stata consegnata con contenuto DIVERSO (%s, ora %s). Un client Download Play che ha gia' il figlio di quella "
                 "versione risponde 'ho gia' il contenuto' e rilancia la VECCHIA copia. Eseguire: version.py bump micro" % (v, same[0][2][:16], digest[:16]))
    if not same and "--check-only" in sys.argv[2:]:
        print("versione %d non ancora registrata in %s: controllo soltanto (--register la registra alla consegna) [%s]" % (v, tsv, brief))
    elif not same:
        open(tsv, "a").write("%d\t%d.%d.%d\t%s\t%s\t%s\n" % (v, *read(), digest, datetime.date.today().isoformat(), brief))
        print("registrata la versione %d in %s [%s]" % (v, tsv, brief))
    else:
        print("versione %d gia' registrata con gli stessi tre file: ok [%s]" % (v, brief))
else:
    sys.exit(__doc__)
