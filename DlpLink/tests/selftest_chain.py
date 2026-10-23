#!/usr/bin/env python3
"""selftest_chain.py MODELLO.bin -- il test negativo di check_chain.py: un controllo che non sa dire NO non controlla niente.

Prende il modello vero della costruzione (DlpLink/out/child_model.bin) e ne fa copie con UN guasto ciascuna (il checksum rifatto, cosi' restano modelli validi), poi chiede a
check_chain.py di giudicarle: tre guasti che DEVE rifiutare (un byte dell'installatore diverso, un'altra versione, un'immagine della guida che non c'e') e uno che DEVE accettare
(un byte della chiave del biglietto, che makerom tira a sorte a ogni esecuzione: e' proprio il caso di due figli veri dello stesso installatore). Esce con 1 se uno di questi
cinque verdetti e' sbagliato.

Lo stesso per il plugin che l'app porta come risorsa (`check_chain.py plugin`): col plugin vero della costruzione (AzaharLink/AzaharLink.3gx) e un finto "ELF" che lo contiene deve
ACCETTARE, e deve RIFIUTARE un'altra versione, una copia di data/ che differisce di UN byte, un'app che porta il plugin con un byte diverso (costruita con quello di prima), un'app che lo
porta tagliato e un file che non e' un .3gx.
"""
import hashlib
import os
import struct
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.join(HERE, "..", "tools")
sys.path.insert(0, TOOLS)
import make_model as mm  # noqa: E402

CHK = os.path.join(TOOLS, "check_chain.py")
PLUGIN = os.path.join(HERE, "..", "..", "AzaharLink", "AzaharLink.3gx")
VERSION_FILE = os.path.join(HERE, "..", "..", "AzaharLink", "VERSION")
failed = 0


def run(*args):
    r = subprocess.run([sys.executable, CHK, *args], capture_output=True, text=True)
    return r.returncode, r.stdout + r.stderr


def expect(name, got, want, out):
    global failed
    ok = (got == 0) == want
    print(("OK   " if ok else "FAIL ") + name + " (check_chain.py: " + ("accepts" if got == 0 else "refuses") + ", " + ("must accept" if want else "must refuse") + ")")
    if not ok:
        failed += 1
        print("     " + out.strip().replace("\n", "\n     ")[:600])


def rechecksum(m):
    m = bytearray(m)
    m[mm.HASH_AT:mm.HASH_AT + 32] = hashlib.sha256(bytes(m[:mm.HASH_AT]) + bytes(m[mm.HDR:])).digest()
    return bytes(m)


def plugin_checks(tmp):
    """The negative test of the plugin check: a made-up app that carries the real plugin of the build, and the five ways it can be wrong."""
    if not os.path.exists(PLUGIN) or not os.path.exists(VERSION_FILE):
        print("     (no AzaharLink.3gx in the tree: the check of the plugin is not tried)")
        return
    plugin = open(PLUGIN, "rb").read()
    version = open(VERSION_FILE).read().strip()
    major, minor, micro = (int(x) for x in version.split("."))
    other_version = "%d.%d.%d" % (major, minor, micro + 1 if micro < 15 else micro - 1)

    def save(name, data):
        path = os.path.join(tmp, name)
        open(path, "wb").write(data)
        return path

    def app_with(blob):
        return save("app_%d.bin" % len(blob), bytes(range(256)) * 64 + blob + b"the rest of the app")

    flipped = bytearray(plugin)
    flipped[len(plugin) // 2] ^= 0x01
    app_ok = app_with(plugin)
    rc, out = run("plugin", PLUGIN, "--version", version, "--copy", PLUGIN, "--in", app_ok)
    expect("an app that carries the plugin of the build, with its version and the same copy in data/, is accepted", rc, True, out)
    rc, out = run("plugin", PLUGIN, "--version", other_version, "--copy", PLUGIN, "--in", app_ok)
    expect("a plugin whose header says another version than VERSION is refused", rc, False, out)
    rc, out = run("plugin", PLUGIN, "--version", version, "--copy", save("copy_flipped.3gx", bytes(flipped)), "--in", app_ok)
    expect("a copy in data/ that differs from the plugin of the build by ONE byte is refused", rc, False, out)
    rc, out = run("plugin", PLUGIN, "--version", version, "--copy", PLUGIN, "--in", app_with(bytes(flipped)))
    expect("an app that carries the plugin with ONE byte different (built with the plugin before) is refused", rc, False, out)
    rc, out = run("plugin", PLUGIN, "--version", version, "--copy", PLUGIN, "--in", app_with(plugin[:-1]))
    expect("an app that carries the plugin cut short by one byte is refused", rc, False, out)
    rc, out = run("plugin", PLUGIN, "--version", version, "--copy", PLUGIN, "--in", save("app_without.bin", bytes(range(256)) * 64))
    expect("an app that carries no plugin at all is refused", rc, False, out)
    not_3gx = save("not_a_plugin.3gx", b"NOT-A-3GX" + plugin[9:])
    rc, out = run("plugin", not_3gx, "--version", version, "--copy", not_3gx, "--in", app_with(open(not_3gx, "rb").read()))
    expect("a file that is not a .3gx is refused as the plugin", rc, False, out)


def main():
    model_path = sys.argv[1]
    model = open(model_path, "rb").read()
    info = mm.parse_model(model)
    L = mm.cia_layout(model[mm.HDR:mm.HDR + info["head"]])
    with tempfile.TemporaryDirectory() as tmp:
        def save(name, data):
            path = os.path.join(tmp, name)
            open(path, "wb").write(data)
            return path

        # a byte of the installer (inside the ExeFS), the checksum made again: another model, not another run of makerom
        code = bytearray(model)
        exefs_off = mm.le32(model, mm.HDR + L["oc"] + 0x1A0) * 0x200      # (the ExeFS starts where the NCCH header says)
        code[mm.HDR + L["oc"] + exefs_off + 0x2000] ^= 0xFF
        # a byte of the title key of the ticket: what two runs of makerom draw differently
        key = bytearray(model)
        key[mm.HDR + L["tb"] + 0x7F] ^= 0x55
        # the next micro in the TMD, the ticket and the header
        ver = bytearray(model)
        nv = info["version"] + 1 if (info["version"] & 15) < 15 else info["version"] - 1
        struct.pack_into(">H", ver, mm.HDR + L["hb"] + 0x9C, nv)
        struct.pack_into(">H", ver, mm.HDR + L["tb"] + 0xA6, nv)
        struct.pack_into("<H", ver, 0x10, nv)
        p_code, p_key, p_ver = (save(n, rechecksum(d)) for n, d in (("code.bin", code), ("key.bin", key), ("ver.bin", ver)))
        rc, out = run("agree", model_path, model_path)
        expect("a model agrees with itself", rc, True, out)
        rc, out = run("agree", model_path, p_key)
        expect("two models that differ in the title key of the ticket (random in every run of makerom) agree", rc, True, out)
        rc, out = run("agree", model_path, p_code)
        expect("two models that differ in a byte of the installer do NOT agree", rc, False, out)
        rc, out = run("model", p_ver, "--version16", str(info["version"]))
        expect("a model of another version is NOT the model of this version", rc, False, out)
        missing = save("picture.t3x", b"not a picture of the guide, and not in any model" * 8)
        rc, out = run("carries", missing, "--in", model_path)
        expect("a picture that is not in the installer is NOT found", rc, False, out)
        plugin_checks(tmp)
    print("selftest_chain: %s" % ("every verdict is right" if failed == 0 else "%d wrong verdict(s)" % failed))
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
