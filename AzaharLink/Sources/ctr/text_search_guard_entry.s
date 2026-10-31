@ Entry of the Process::GetTextSize() guard (see text_search_guard.cpp; this file is named _entry so that its object does not collide with the C++ one).
@
@ -Wl,--wrap=_ZN18CTRPluginFramework7Process11GetTextSizeEv sends every call of the framework's
@ Process::GetTextSize() here. The function takes no argument, so r0 and r1 are free: they carry
@ what the C++ side needs. r6 is callee-saved, so on entry it still holds the caller's value, and
@ lr is left alone: the C++ function is reached by a plain branch and returns straight to the caller.

    .arm
    .text
    .align 2

    .global __wrap__ZN18CTRPluginFramework7Process11GetTextSizeEv
    .type __wrap__ZN18CTRPluginFramework7Process11GetTextSizeEv, %function
__wrap__ZN18CTRPluginFramework7Process11GetTextSizeEv:
    mov  r0, r6
    mov  r1, lr
    b    azl_text_size_guard
    .size __wrap__ZN18CTRPluginFramework7Process11GetTextSizeEv, . - __wrap__ZN18CTRPluginFramework7Process11GetTextSizeEv
