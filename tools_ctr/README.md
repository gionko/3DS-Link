# tools_ctr

The build chain needs two tools of [Project_CTR](https://github.com/3DSGuy/Project_CTR), as **Linux binaries** run inside Docker:

* `makerom` v0.19.0 — makes the CIA of the app (and the reference CIAs the tests compare with).
* `ctrtool` — checks a CIA from outside.

They are **not** in the repository. Compile them once:

```bash
bash tools_ctr/build_tools.sh        # clones Project_CTR at the commit below and builds both inside the devkitpro/devkitarm image
```

Commit used: `d52c2b0e788c2d085763d7c5dcc4b2edd9ffb7d8` (makerom v0.19.0). Any makerom 0.19.x and a recent ctrtool placed in this folder as `makerom` and `ctrtool` also work; `DlpLink/tools/verify_cia.py` checks every CIA that comes out.

makerom is MIT-licensed; the folder of ctrtool in Project_CTR states no license, which is why the binaries are not redistributed here. Neither tool's code is in 3DS Link (see `AzaharLink/CREDITS.md`).

`LEGGIMI.txt` (Italian) is the note of the maintainer about the binaries he used.
