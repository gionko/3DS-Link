#!/usr/bin/env python3
"""dlpc_model_agree.py TOOLS_DIR DIR -- per ogni DIR/*.bin stampa "nome ok" o "nome bad": il verdetto di make_model.py (parse_model) sul modello.

Lo usa DlpLink/tests/dlpc_test.c per provare che il controllo del compositore (dlpc.c, model_parse) e quello di make_model.py (--check) applicano le STESSE
regole: stessi modelli accettati e stessi rifiutati, anche quando la somma di controllo e' rifatta (rimane solo la struttura).
"""
import os
import sys

sys.dont_write_bytecode = True                  # (nessun __pycache__ dentro DlpLink/tools)
sys.path.insert(0, sys.argv[1])
import make_model  # noqa: E402

for name in sorted(os.listdir(sys.argv[2])):
    if not name.endswith(".bin"):
        continue
    try:
        make_model.parse_model(open(os.path.join(sys.argv[2], name), "rb").read())
        print(name, "ok")
    except ValueError:
        print(name, "bad")
