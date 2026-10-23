// The demo takes the IV of the checksum the way the 3DS Link app does: from the built-in keys module of the app (the value Azahar ships as dlpChecksumModIv),
// compiled here as it is, with the AES it uses (encryption and CTR only).
#include "../../../../AzaharLink/ConfigApp/titleshare/common/aes.c"
#include "../../../../AzaharLink/ConfigApp/titleshare_host/builtin_keys.c"
