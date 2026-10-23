# Changelog

3DS Link has one version number for everything (plugin, app, CIA, installer, documents). The long history, with the hardware tests of each version, is in `AzaharLink/README.md` (Italian).

## 1.0.3 - 2026-10-23
* The constant of the hardware key generator and the IV of the Download Play checksum are carried in the program, obfuscated with a key of zeros, the way Azahar carries its default keys; they were compiled in as plain values. The Download Play library no longer carries the IV: its caller gives it (`dlph_cfg.iv`), and it refuses to open without one.
* A test pins both values (by their hash) and what the key generator makes with three pairs of keys.
* The example serial in the help text of the guide is a made-up one.
* The Download Play child carries the access descriptor that `makerom` makes itself, with the wildcard programId (tried on a console: a console without 3DS Link downloaded and installed it). Nothing of Nintendo's is needed to build any more, and the release carries the package.
* Nothing else changes for the user. The banner, the HOME menu entry and the rest are as in 1.0.2.

## 1.0.2 - 2026-10-22
* HOME menu: the package shows the name "3DS Link" and the publisher (not the slogan) and has a banner with a tune.
* *3DS Link Delivery* shows the Wi-Fi channel ("CH n") like *Titles Delivery*.
* Installer: on the finished screen only A and B leave, Y opens the guide, the other buttons do nothing; the log says who made it leave.
* Slogan: "Connect, share, play together".
* The guide also answers the Circle Pad; long messages stay up long enough to read; the plugin's `.old` leftover of a power cut is removed; credits for ENet and bannertool.
* A diagnostic build of the plugin for games that close at launch (`AzaharLink/diag`).

## 1.0.1 - 2026-10-22
* One package: the plugin is inside the app, which writes it to `sdmc:/3ds/3DSLink/` and compares it byte for byte at every start; the main switch cannot be turned on without it.
* New *About* page ("Created by Gionko", how the app was installed, the plugin row) and a *Credits and thanks* page.
* The "How to get boot9.bin" guide moves with up/down.

## 1.0.0 - 2026-10-21
* First numbered version: the app as a CIA (and 3DSX), *Titles Delivery*, *3DS Link Delivery* (the app handed over with Download Play alone), the installation check, the first-start guide.
