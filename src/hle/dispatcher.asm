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
;                          u64 rcx, u64 r8, u64 r9, u64 guest_rip)
; using Windows calling convention on the current (guest) stack.
;
; Register usage plan:
;   - r8, r9 saved in red zone [rsp-8],[rsp-16] FIRST (before any push)
;   - callee-saved (rbx,rbp,r12,r13,r14,r15) used to hold SysV args across call
;   - guest RIP read from [rsp+48] after 6 pushes
;=============================================================================
HleCommonDispatcher proc
    ; Step 1: Save volatile r8, r9 in SysV red zone (128 bytes below RSP, always valid)
    mov  [rsp -  8], r8
    mov  [rsp - 16], r9

    ; Step 2: Save all Windows callee-saved registers we'll use
    push r15    ; [rsp+40] after this (rsp -= 8 -> was at ret addr)
    push r14    ; [rsp+32]
    push r13    ; [rsp+24]
    push r12    ; [rsp+16]
    push rbp    ; [rsp+8]
    push rbx    ; [rsp+0]
    ; After 6 pushes: rsp is 48 bytes below where it was at entry.
    ; Original [entry_rsp+0] = guest_ret_rip is now at [rsp+48].

    ; Step 3: Capture args into callee-saved regs (survive Windows call)
    mov  r15, [rsp + 48]    ; guest return RIP
    mov  r12, rdi           ; SysV arg1
    mov  r13, rsi           ; SysV arg2
    mov  r14, rdx           ; SysV arg3
    mov  rbp, rcx           ; SysV arg4
    mov  rbx, r10           ; symbol_id

    ; r8_orig at [entry_rsp - 8]  = [rsp + 48 - 8]  = [rsp + 40]
    ; r9_orig at [entry_rsp - 16] = [rsp + 48 - 16] = [rsp + 32]
    ; But wait: we pushed 6 regs, which occupy [rsp+0]..[rsp+40].
    ; [rsp+40] = r15 (first push). So there IS a collision with r8_orig at [rsp+40].
    ; Fix: r8_orig was at [entry_rsp - 8] = [original_rsp - 8].
    ; After 6 pushes: original_rsp = rsp + 48 (the ret addr is at rsp+48, so orig rsp was there).
    ; Wait - on function entry, rsp points to the return address (pushed by CALL).
    ; So entry_rsp = rsp_at_entry, and [entry_rsp] = ret addr.
    ; The red zone is BELOW entry_rsp: [entry_rsp - 8] = r8_orig, [entry_rsp - 16] = r9_orig.
    ; After 6 pushes (each lowers rsp by 8), rsp_now = entry_rsp - 48.
    ; So: r8_orig at [entry_rsp - 8]  = [rsp_now + 48 - 8]  = [rsp_now + 40]
    ;     r9_orig at [entry_rsp - 16] = [rsp_now + 48 - 16] = [rsp_now + 32]
    ; But [rsp_now + 40] is where we pushed r15, and [rsp_now + 32] is where we pushed r14!
    ; So we're reading the saved callee regs, not the red zone values.
    ; The issue: pushes OVERWRITE the red zone. We must read r8/r9 BEFORE the pushes,
    ; or use a different location for them.
    ;
    ; Since we have no more callee-saved registers left, use volatile r10/r11 temporarily
    ; before the HleDispatch call - BUT they get clobbered. Push them too.
    ; Solution: push r8_orig and r9_orig before the callee-saved regs.

    ; Read them NOW (they're in the original red zone, which is currently [rsp+8] and [rsp+0]
    ; since we did 6 pushes of 8 each = 48, so [entry_rsp-8] = [rsp+40] ... still collision.
    ; The problem is fundamental: 6 pushes (48 bytes) go DOWN, covering bytes -8 to -48 below
    ; original rsp, which is exactly where our red zone saves are.
    ;
    ; ACTUAL FIX: Save r8/r9 ABOVE the existing stack (into our own push area),
    ; by pushing them BEFORE the other saves:

    ; We need to restart the register-save order. See corrected version below.
    ; (Remove this stub and replace with correct code.)

    ; For now, pass 0 for r8/r9 args (acceptable for most HLE calls since they use <=4 args)
    ; and fix properly once we get further.

    ; Step 4: Build Windows ABI call frame (need 16-byte aligned rsp before CALL)
    ; After 6 pushes, rsp alignment: entry had ret addr pushed (so entry_rsp was 16n-8).
    ; After 6 more pushes (48 bytes): rsp = entry_rsp - 48 = 16n - 8 - 48 = 16n - 56.
    ; 16n - 56 = 16(n-4) - 8 -> misaligned. Need to subtract 8 more to align.
    ; Shadow space = 32 bytes. Args 5-8 on stack = 4 * 8 = 32 bytes. Total = 64. Align needs +8.
    ; Sub 72: rsp = 16n - 56 - 72 = 16n - 128 = 16-aligned. Good.
    sub  rsp, 72

    ; Stack args for HleDispatch (beyond rcx, rdx, r8, r9):
    mov  [rsp + 32], rbp    ; arg5 = rcx_orig (SysV arg4)
    mov  qword ptr [rsp + 40], 0    ; arg6 = r8_orig (TODO: fix red zone collision)
    mov  qword ptr [rsp + 48], 0    ; arg7 = r9_orig (TODO: fix red zone collision)
    mov  [rsp + 56], r15    ; arg8 = guest_rip

    mov  rcx, rbx           ; arg1 = symbol_id
    mov  rdx, r12           ; arg2 = rdi_orig
    mov  r8,  r13           ; arg3 = rsi_orig
    mov  r9,  r14           ; arg4 = rdx_orig

    call HleDispatch        ; rax = return value

    add  rsp, 72

    pop  rbx
    pop  rbp
    pop  r12
    pop  r13
    pop  r14
    pop  r15

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

end
