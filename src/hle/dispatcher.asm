;=============================================================================
; dispatcher.asm - PCSX5 HLE guest/host call dispatcher
;
; HleCommonDispatcher:
;   Entry: r10 = symbol_id, rdi/rsi/rdx/rcx/r8/r9 = SysV guest args
;          [rsp+0] = guest return address (pushed by import thunk CALL)
;   Calls HleDispatch (Windows ABI) and returns rax to the guest.
;
; StartGuest:
;   Windows ABI: rcx=entry, rdx=stack_top
;   Switches to guest stack, zeroes registers, jumps to entry. Never returns.
;
; InvokeGuestFunction:
;   Windows ABI: rcx=entry, rdx=rdi_arg, r8=rsi_arg, r9=rdx_arg
;   Sets per-thread host RSP, calls guest function with SysV args, returns rax.
;=============================================================================

.code

extern HleDispatch         : proc
extern SetHostStackPointer : proc

;=============================================================================
; HleCommonDispatcher
;
; Stack on entry (rsp = guest stack, pointing at guest return address):
;   [rsp]    = guest return RIP (pushed by import thunk CALL)
;   below: guest stack frame
;
; We will call HleDispatch(u64 symbol_id, u64 rdi, u64 rsi, u64 rdx,
;                          u64 rcx, u64 r8, u64 r9, u64 guest_rip,
;                          u64 guest_rsp)
; using Windows calling convention on the current (guest) stack.
;
; Register usage plan:
;   - r8, r9 pushed FIRST (before callee-saved regs) so the later pushes don't
;     clobber the SysV red zone where they used to be saved
;   - callee-saved (rbx,rbp,r12,r13,r14,r15) used to hold SysV args across call
;   - guest RIP read from [rsp+64] after 8 pushes
;=============================================================================
HleCommonDispatcher proc
    ; Step 1: Push r8/r9 first (their old red-zone slots would be overwritten
    ; by the callee-saved pushes below, so they must live on the stack too)
    push r8     ; [rsp+56] after all pushes
    push r9     ; [rsp+48] after all pushes

    ; Step 2: Save all Windows callee-saved registers we'll use
    push r15    ; [rsp+40]
    push r14    ; [rsp+32]
    push r13    ; [rsp+24]
    push r12    ; [rsp+16]
    push rbp    ; [rsp+8]
    push rbx    ; [rsp+0]
    ; After 8 pushes: rsp is 64 bytes below where it was at entry.
    ; Original [entry_rsp+0] = guest_ret_rip is now at [rsp+64].

    ; Step 3: Capture args into callee-saved regs (survive Windows call)
    mov  r15, [rsp + 64]    ; guest return RIP
    mov  r12, rdi           ; SysV arg1
    mov  r13, rsi           ; SysV arg2
    mov  r14, rdx           ; SysV arg3
    mov  rbp, rcx           ; SysV arg4
    mov  rbx, r10           ; symbol_id

    ; Step 4: Build Windows ABI call frame (need 16-byte aligned rsp before CALL)
    ; entry_rsp was 16n-8 (ret addr pushed by guest CALL). After 8 pushes
    ; (64 bytes): rsp = 16n - 72 -> misaligned. Shadow space = 32 bytes,
    ; stack args 5-9 = 40 bytes + 8 pad; sub 88: rsp = 16n - 160 = 16-aligned.
    sub  rsp, 88

    ; Stack args for HleDispatch (beyond rcx, rdx, r8, r9).
    ; r8_orig/r9_orig were pushed before the callee-saved regs; after sub 88
    ; they live at [rsp + 88 + 56] and [rsp + 88 + 48].
    mov  [rsp + 32], rbp    ; arg5 = rcx_orig (SysV arg4)
    mov  rax, [rsp + 144]   ; r8_orig
    mov  [rsp + 40], rax    ; arg6 = r8_orig (SysV arg5)
    mov  rax, [rsp + 136]   ; r9_orig
    mov  [rsp + 48], rax    ; arg7 = r9_orig (SysV arg6)
    mov  [rsp + 56], r15    ; arg8 = guest_rip
    lea  rax, [rsp + 152]   ; entry guest rsp (ret addr at [rax], stack args at [rax+8])
    mov  [rsp + 64], rax    ; arg9 = guest_rsp

    mov  rcx, rbx           ; arg1 = symbol_id
    mov  rdx, r12           ; arg2 = rdi_orig
    mov  r8,  r13           ; arg3 = rsi_orig
    mov  r9,  r14           ; arg4 = rdx_orig

    call HleDispatch        ; rax = return value

    ; Mirror the return value into xmm0 so guest code calling HLE functions
    ; with floating-point results (strtod, strtof, ...) sees the value where
    ; the SysV ABI puts it.  Integer/pointer callers ignore xmm0, so this is
    ; always safe.
    movq xmm0, rax

    add  rsp, 88

    pop  rbx
    pop  rbp
    pop  r12
    pop  r13
    pop  r14
    pop  r15
    add  rsp, 16            ; drop saved r8/r9

    ret                     ; return rax to guest
HleCommonDispatcher endp

;=============================================================================
; StartGuest
; Windows ABI: rcx = entry_point, rdx = guest_stack_top
; This function DOES NOT RETURN to its caller.
;=============================================================================
StartGuest proc
    ; We need to: (1) call SetHostStackPointer, (2) switch rsp to guest stack, (3) jmp entry.
    ; Since this is a non-returning function, we can freely use any register after step (1).
    ;
    ; Save entry (rcx) and stack_top (rdx) in non-volatile temp regs first.
    ; We only need ONE Windows call (SetHostStackPointer), so we just need the standard prologue,
    ; but we DON'T pop the saved regs — we unwind rsp directly with ADD.

    ; Save regs we'll need after the call in callee-saved slots:
    push rbx                ; rbx will hold entry_point
    push r12                ; r12 will hold stack_top
    sub  rsp, 32            ; shadow for SetHostStackPointer

    mov  rbx, rcx           ; entry_point
    mov  r12, rdx           ; stack_top

    lea  rcx, [rsp]         ; current host rsp for SetHostStackPointer
    call SetHostStackPointer

    ; Now restore rbx and r12 (they still hold our values, pop just adjusts rsp):
    add  rsp, 32            ; remove shadow
    ; DO NOT pop r12 and rbx here — the pop would restore them to caller's values!
    ; Instead, rbx and r12 still hold OUR entry_point and stack_top.
    ; Just unwind rsp by 16 (the two pushes we did).
    add  rsp, 16            ; undo push rbx + push r12 (without actually popping)
    ; Skip the return address on the caller's stack too:
    add  rsp, 8             ; skip return address (we're never returning)

    ; rbx = entry_point, r12 = stack_top (still valid — add rsp doesn't change reg values)
    ; Switch to guest stack
    mov  rsp, r12

    ; PS5 _start receives its first argument (rdi) pointing to the stack top
    ; where argc/argv/envp are laid out. Set rdi = rsp per PS5 ABI.
    mov  rdi, rsp

    ; SysV ABI expects rsp % 16 == 8 at function entry (as if reached via CALL).
    ; The compiled _start relies on this to keep 16-byte alignment at its own
    ; call sites; without it, SSE spills (movdqa) in downstream code fault.
    sub  rsp, 8

    ; Zero other registers (but NOT rdi or rsp which we just set)
    xor  r12, r12
    xor  r13, r13
    xor  r14, r14
    xor  r15, r15
    xor  rbp, rbp
    xor  rax, rax
    xor  rcx, rcx
    xor  rdx, rdx
    xor  rsi, rsi
    xor  r8,  r8
    xor  r9,  r9
    xor  r10, r10
    xor  r11, r11

    jmp  rbx                ; jump to entry_point (rbx was not zeroed)
StartGuest endp

;=============================================================================
; InvokeGuestFunction
; Windows ABI: rcx=entry, rdx=rdi_arg, r8=rsi_arg, r9=rdx_arg
; Returns: rax = guest return value
;=============================================================================
InvokeGuestFunction proc
    push rbx
    push rbp
    push r12
    push r13
    push r14
    push r15
    sub  rsp, 40

    mov  rbx, rcx           ; entry_point
    mov  r12, rdx           ; rdi_arg
    mov  r13, r8            ; rsi_arg
    mov  r14, r9            ; rdx_arg

    ; Update per-thread host RSP so HleCommonDispatcher can switch back if needed
    lea  rcx, [rsp]
    call SetHostStackPointer

    ; Set up SysV args and call guest
    mov  rdi, r12
    mov  rsi, r13
    mov  rdx, r14
    xor  rcx, rcx
    xor  r8,  r8
    xor  r9,  r9
    xor  rax, rax

    call rbx                ; rax = guest return value

    add  rsp, 40
    pop  r15
    pop  r14
    pop  r13
    pop  r12
    pop  rbp
    pop  rbx
    ret
InvokeGuestFunction endp

;=============================================================================
; InvokeGuestFunction6
; Windows ABI: rcx=entry, rdx=pointer to 6 u64 SysV args (rdi,rsi,rdx,rcx,r8,r9)
; Used by the HLE C++ exception unwinder to invoke guest personality routines
; (5 args) and guest exception destructors. Returns: rax = guest return value
;=============================================================================
InvokeGuestFunction6 proc
    push rbx
    push rbp
    push r12
    push r13
    push r14
    push r15
    sub  rsp, 40

    mov  rbx, rcx           ; entry_point
    mov  r12, rdx           ; args pointer

    ; Update per-thread host RSP so HleCommonDispatcher can switch back if needed
    lea  rcx, [rsp]
    call SetHostStackPointer

    ; Load SysV args from the array (order: rdi,rsi,rdx,rcx,r8,r9)
    mov  rdi, [r12]
    mov  rsi, [r12 + 8]
    mov  rdx, [r12 + 16]
    mov  rcx, [r12 + 24]
    mov  r8,  [r12 + 32]
    mov  r9,  [r12 + 40]
    xor  rax, rax

    call rbx                ; rax = guest return value

    add  rsp, 40
    pop  r15
    pop  r14
    pop  r13
    pop  r12
    pop  rbp
    pop  rbx
    ret
InvokeGuestFunction6 endp

end
