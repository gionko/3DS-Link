# Crediti e ringraziamenti di 3DS Link

**3DS Link** (il nome dell'app; «Connect, share, play together» è il suo slogan) è stato creato da **Gionko**. Poggia sul lavoro di molti altri: quello che usa, quello che ha copiato e quello da cui ha imparato è elencato qui sotto, con l'autore, la licenza e dove vive. L'elenco è lo stesso che l'app mostra in **About 3DS Link > Credits and thanks** (è scritto una volta sola in `ConfigApp/source/about_text.hpp`; `Tests/settings_test/about_test.cpp` legge questo file e controlla che ogni nome, licenza, autore, testo e indirizzo dell'elenco sia qui, parola per parola: se uno dei due cambia e l'altro no, il test fallisce).

3DS Link è software libero, **GPL v2 o successiva**: la logica UDS del plugin è un port di codice di Azahar (che a sua volta viene da Citra), che è «GPLv2 or any later version». ENet resta sotto la sua licenza MIT (`third_party/enet/LICENSE`).

Le voci sono in inglese perché sono le righe che l'app mostra; le licenze sono quelle che ogni progetto dichiara nel suo repository (o nel file che ne porta il testo, dove l'abbiamo letto), controllate il 2026-10-22. Se una licenza qui è sbagliata o è cambiata, vale quella del progetto originale.

## The list

### Azahar Emulator

- What for: The room 3DS Link connects to, and the model for everything it does with one. The plugin's UDS logic (nwm::UDS, frames, beacons, ndm) is a port of Azahar's nwm_uds, uds and ndm code. Its Download Play code was read to build 3DS Link Delivery.
- By: the Azahar and Citra Emulator Projects
- License: GPL-2.0+ - GPL v2 or later (the files ported from)
- Link: https://github.com/azahar-emu/azahar

### FBI

- What for: The reference for installing a title: the way Titles Delivery and the installer talk to the AM service (begin, write, finish, seed, clean up what a failed install leaves), the layout of a CIA, how a title's name and icon are read. 3DS Link follows FBI's sequence.
- By: Steveice10
- License: MIT - MIT license
- Link: https://github.com/Steveice10/FBI

### 3dbrew

- What for: The documentation of the 3DS this was built from: the services (nwm::UDS, AM, FS, soc), the formats (CIA, TMD, NCCH, SMDH, 3GX) and the Download Play protocol. Its tables and notes are cited in the comments of the code.
- By: the 3dbrew community
- License: wiki - none stated on the site: used as documentation
- Link: https://3dbrew.org

### devkitPro

- What for: The toolchain that builds the app, the plugin and the installer: devkitARM (compiler, linker), 3dsxtool and smdhtool (the .3dsx and the HOME menu entry), tex3ds by mtheall (the guide's pictures). Build tools only: none of their code is in 3DS Link.
- By: Dave Murphy, fincs and the devkitPro team
- License: GPL - GPL (3dstools and tex3ds: GPL v3)
- Link: https://devkitpro.org

### libctru

- What for: The 3DS system library under everything: services, sockets, UDS, AM, graphics, input.
- By: smealum, fincs and the libctru contributors
- License: zlib - zlib license
- Link: https://github.com/devkitPro/libctru

### citro2d, citro3d

- What for: All the drawing of the app and of the installer: the lists, the notes, the guide's pictures.
- By: fincs and the devkitPro contributors
- License: zlib - zlib license
- Link: https://github.com/devkitPro/citro2d

### Luma3DS

- What for: What loads AzaharLink.3gx into a game: its Plugin Loader (the plg:ldr service the app asks about and switches on) and svcControlService, which the plugin uses. csvc.h is adapted from Luma3DS's own.
- By: the LumaTeam (AuroraWright, TuxSH and the contributors)
- License: GPL-3.0 - GPL v3 (csvc.h: zlib-style terms in the file)
- Link: https://github.com/LumaTeam/Luma3DS

### CTRPluginFramework

- What for: The framework AzaharLink.3gx is built on (menu, process events, patches, files) and the template the project started from: 3gx.ld, csvc.h, types.h and Unicode.h are its.
- By: Nanquitas, The Pixellizer Group; PabloMK7's 0.8.0 build
- License: custom - its own: free to use, the source going with its binary
- Link: https://github.com/PabloMK7/CTRPluginFramework-BlankTemplate

### ENet

- What for: The reliable UDP library the room speaks (version 1.3.18). It is in the plugin and in the app; enet_3ds.c, the layer on the 3DS sockets, is derived from ENet's own unix.c.
- By: Copyright (c) 2002-2024 Lee Salzman
- License: MIT - MIT license
- Link: https://github.com/lsalzman/enet

### GodMode9

- What for: The guide "How to get boot9.bin" shows GodMode9 v2.1.1: its pictures are cropped screenshots with arrows. The CIA code builds the certificate chain the way GodMode9 does. GM9Megascript, the script the guide has you run, is by annson24.
- By: d0k3 and the GodMode9 contributors
- License: GPL-3.0 - GPL v3
- Link: https://github.com/d0k3/GodMode9

### makerom, ctrtool

- What for: Build and check tools (makerom 0.19.0): makerom builds the CIA of the app and the reference ones the tests compare with, ctrtool checks a CIA from outside. None of their code is in 3DS Link.
- By: 3DSGuy, Jakcron and the Project_CTR contributors
- License: MIT - makerom: MIT; ctrtool: its folder of the repository states no license
- Link: https://github.com/3DSGuy/Project_CTR

### bannertool

- What for: Makes the banner of the HOME menu (the small 3D model and its tune) from banner.png and jingle.wav. It is fetched by DlpLink/tools/bannertool/build.sh and runs on the build machine only: none of it is in the app.
- By: Steveice10; stb libraries by Sean Barrett
- License: MIT - MIT license (stb_image, stb_vorbis: public domain or MIT)
- Link: https://github.com/Mtgxyz/bannertool

### Pillow

- What for: Draws the icon and the guide's pictures: a build script, not part of the app.
- By: the Pillow contributors
- License: HPND - HPND (the Pillow license)
- Link: https://github.com/python-pillow/Pillow

## Also

- **Titles Delivery** (the module under it, `ConfigApp/titleshare/`) comes from TitleShare, an earlier experiment of the same author; `ConfigApp/titleshare/PATCHES.md` lists every change made to it.
- **3DS Link Delivery** (the Download Play host, `ConfigApp/dlph/`, and the child it hands out) was written for this project from the public protocol (3dbrew, and Azahar's Download Play code as a reference) and tried on real consoles.
- **MD5, AES and SHA-256** (`Sources/azl/crypto.cpp`, `ConfigApp/titleshare/common/aes.c` and `sha256.c`) are implementations of the public standards (RFC 1321, FIPS 197, FIPS 180-4); the files carry no third-party notice.
- **The icon** and **the guide's pictures** are drawn for this project (`ConfigApp/tools/make_icon.py`, `DlpLink/tools/make_guide.py`); the guide's pictures are crops of GodMode9 v2.1.1 screenshots, with arrows drawn on them.
- **Nintendo 3DS** and the games that run on it belong to Nintendo and to their makers. 3DS Link is not made, endorsed or supported by Nintendo, by Azahar or by any project in this list.
