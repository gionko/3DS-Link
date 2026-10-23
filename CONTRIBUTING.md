# Contributing

Thanks for looking. Issues and pull requests are welcome.

## Reporting a problem

Open an issue and say: the **3DS Link version** (About page, or `AzaharLink/VERSION`), the console (New 3DS / New 2DS XL / Old...), the **Luma3DS version** (*Settings* > *Installation check* in the app shows it), the game and its region, what you did and what happened. For a game that closes or freezes, the files `AzaharLink.log` and, with `debug=1`, `AzaharLink.debug.log` from `sd:/3ds/3DSLink/` are what help most. **Read them before you post: they contain the MAC address of your console and the names of the players in the room.**

## Sending a change

* Fork, branch, change, open a pull request. Say what it does and what you tried it on (a PC test is welcome, a console test is better, "not tried on a console" is an honest answer).
* The tests run in Docker; see the README. At least run `AzaharLink/Tests/policy_check.sh` (it reads the sources and fails when a rule that decides what the plugin does changes by accident) and `AzaharLink/Tests/run.sh`. A new rule gets a pin in `policy_check.sh` and a mutant in `policy_mutants.py`.
* **One version number for everything** (`AzaharLink/VERSION`): `python AzaharLink/Tests/set_version.py X.Y.Z` rewrites every point that carries it; do not bump it by hand and do not bump it for a change that is not a release.
* The plugin runs inside someone else's game with a few kilobytes of stack on some threads: read the "game-thread code" markers before touching `Sources/ctr/hook.cpp`.
* **Never add** game files, firmware or system dumps, `boot9.bin`/`boot11.bin`/`movable.sed`, encryption keys of your own console, signed blobs (the only key material in the repository is the two default values that Azahar carries too, in `builtin_keys_blob.h`: do not add others), text of Nintendo's SDK documents, or logs and captures with real MAC addresses or names. Anything of Nintendo's stays Nintendo's; whoever builds brings it from their own console.

## License

By contributing you agree that your contribution is under the project's license, GPL-2.0-or-later (see `LICENSE`).
