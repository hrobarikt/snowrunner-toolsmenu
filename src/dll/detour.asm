; Entry detour for the frame hook.
;
; The patch jumps here from the first instruction of the hooked per-frame
; function, so every volatile and argument register still holds the original
; caller's values. They are preserved across the queue drain and restored
; before control continues into the trampoline, which runs the stolen bytes and
; jumps back to the rest of the original function.
;
; The entry and exit instructions are what make detach safe. The detour claims
; itself before it touches anything else and releases itself as the
; second-to-last instruction, so the only window where a thread is inside this
; module without being counted is one where its instruction pointer is on the
; module image, which is exactly what detach checks in addition to the count.

EXTERN SrtmFrameDrain : PROC
EXTERN g_trampoline : QWORD
EXTERN g_active_detours : DWORD

.code

SrtmFrameDetour PROC
    ; Claim the detour first, before any register is saved. A thread suspended
    ; between the patched jump and this instruction has not been counted yet,
    ; but its RIP is on this module's image.
    lock inc dword ptr [g_active_detours]

    ; On entry RSP is 8 mod 16 (the caller's return address is pushed).
    ; 0D8h is 8 mod 16, so after this RSP is 16-byte aligned.
    sub     rsp, 0D8h

    movups  xmmword ptr [rsp+000h], xmm0
    movups  xmmword ptr [rsp+010h], xmm1
    movups  xmmword ptr [rsp+020h], xmm2
    movups  xmmword ptr [rsp+030h], xmm3
    movups  xmmword ptr [rsp+040h], xmm4
    movups  xmmword ptr [rsp+050h], xmm5
    mov     qword ptr [rsp+060h], rax
    mov     qword ptr [rsp+068h], rcx
    mov     qword ptr [rsp+070h], rdx
    mov     qword ptr [rsp+078h], r8
    mov     qword ptr [rsp+080h], r9
    mov     qword ptr [rsp+088h], r10
    ; R11 is deliberately not saved: the exit path below overwrites it with the
    ; trampoline address, so saving it here would only be a store nothing reads.

    sub     rsp, 20h                    ; shadow space; RSP stays 16-aligned
    call    SrtmFrameDrain
    add     rsp, 20h

    ; Read the trampoline address into a register while this module is still
    ; provably mapped: after the release below, the indirect jump may no longer
    ; touch module memory. R11 is volatile and carries no argument under any
    ; convention the game's compiler emits, so the original prologue in the
    ; trampoline cannot observe that it was not restored. Every other register
    ; is restored exactly as it arrived.
    mov     r11, qword ptr [g_trampoline]

    mov     r10, qword ptr [rsp+088h]
    mov     r9,  qword ptr [rsp+080h]
    mov     r8,  qword ptr [rsp+078h]
    mov     rdx, qword ptr [rsp+070h]
    mov     rcx, qword ptr [rsp+068h]
    mov     rax, qword ptr [rsp+060h]
    movups  xmm5, xmmword ptr [rsp+050h]
    movups  xmm4, xmmword ptr [rsp+040h]
    movups  xmm3, xmmword ptr [rsp+030h]
    movups  xmm2, xmmword ptr [rsp+020h]
    movups  xmm1, xmmword ptr [rsp+010h]
    movups  xmm0, xmmword ptr [rsp+000h]

    add     rsp, 0D8h

    ; Release the detour as the second-to-last instruction. Nothing after this
    ; may read module memory, because detach can report success and the module
    ; can be unloaded the instant the count drops: the jump goes through a
    ; register, and the trampoline page it lands on is never freed.
    lock dec dword ptr [g_active_detours]
    jmp     r11
SrtmFrameDetour ENDP

END
