; Original synthetic Phase 0 fixture. No guest binaries or SDK input.
; The outer wrapper protects the C++ runner from known legacy ABI violations.
extern InvokeGuestFunction:proc
extern InvokeGuestFunction6:proc
extern InvokeGuestOnStack:proc
extern StartGuest:proc
extern HleCommonDispatcher:proc
extern observed:qword
extern input_args:qword
extern saved_wrapper_rsp:qword
extern shadow_write:qword
extern xmm_input:qword
extern FixtureSetIncomingXmmBlock:proc

; observed offsets match dispatcher_execution.cpp (all fields are uint64_t).
SAVE_HOST macro
    push rbx
    push rbp
    push rdi
    push rsi
    push r12
    push r13
    push r14
    push r15
    sub rsp, 200
    movups [rsp+32], xmm6
    movups [rsp+48], xmm7
    movups [rsp+64], xmm8
    movups [rsp+80], xmm9
    movups [rsp+96], xmm10
    movups [rsp+112], xmm11
    movups [rsp+128], xmm12
    movups [rsp+144], xmm13
    movups [rsp+160], xmm14
    movups [rsp+176], xmm15
endm
RESTORE_HOST macro
    movups xmm6, [rsp+32]
    movups xmm7, [rsp+48]
    movups xmm8, [rsp+64]
    movups xmm9, [rsp+80]
    movups xmm10, [rsp+96]
    movups xmm11, [rsp+112]
    movups xmm12, [rsp+128]
    movups xmm13, [rsp+144]
    movups xmm14, [rsp+160]
    movups xmm15, [rsp+176]
    add rsp, 200
    pop r15
    pop r14
    pop r13
    pop r12
    pop rsi
    pop rdi
    pop rbp
    pop rbx
    ret
endm
SNAPSHOT_NONVOL macro displacement
    mov observed[displacement], rbx
    mov observed[displacement+8], rbp
    mov observed[displacement+16], r12
    mov observed[displacement+24], r13
    mov observed[displacement+32], r14
    mov observed[displacement+40], r15
endm
.code
; Test-owned leaf callback records the real legacy call-site alignment.
; Optional shadow writes model a valid Windows callee's use of home slots.
SetHostStackPointer proc
    mov observed[80], rcx
    mov rax, rsp
    and rax, 15
    mov observed[88], rax
    cmp shadow_write, 0
    je callback_done
    mov qword ptr [rsp+8], 91h
    mov qword ptr [rsp+16], 92h
    mov qword ptr [rsp+24], 93h
    mov qword ptr [rsp+32], 94h
callback_done:
    ret
SetHostStackPointer endp

SetIncomingXmmBlock proc
    sub rsp, 40
    call FixtureSetIncomingXmmBlock
    add rsp, 40
    ; Deterministically destroy volatile lanes so missing restoration is detected.
    pxor xmm1, xmm1
    pxor xmm2, xmm2
    pxor xmm3, xmm3
    pxor xmm4, xmm4
    pxor xmm5, xmm5
    ret
SetIncomingXmmBlock endp

; mode 0=three args, 1=six args, 2=alternate stack, 3=nonreturning start.
RunDispatcherFixture proc
    SAVE_HOST
    mov saved_wrapper_rsp, rsp
    mov r15, rcx
    mov r14, rdx
    mov rdi, 0a1h
    mov rsi, 0a2h
    pcmpeqd xmm6, xmm6
    SNAPSHOT_NONVOL 96
    lea rcx, CaptureGuest
    cmp r15, 3
    je run_start
    cmp r15, 2
    je run_stack
    cmp r15, 1
    je run_six
    mov rdx, input_args[0]
    mov r8, input_args[8]
    mov r9, input_args[16]
    call InvokeGuestFunction
    jmp fixture_return
run_six:
    lea rdx, input_args
    call InvokeGuestFunction6
    jmp fixture_return
run_stack:
    mov rdx, r14
    mov r8, input_args[0]
    call InvokeGuestOnStack
    jmp fixture_return
run_start:
    lea rcx, CaptureStart
    mov rdx, r14
    call StartGuest
    int 3 ; StartGuest must not return through its original call frame.
fixture_return:
    mov observed[56], rax
    mov observed[64], rdi
    mov observed[72], rsi
    movq observed[192], xmm6
    SNAPSHOT_NONVOL 144
    RESTORE_HOST
RunDispatcherFixture endp

CaptureGuest proc
    mov observed[0], rdi
    mov observed[8], rsi
    mov observed[16], rdx
    mov observed[24], rcx
    mov observed[32], r8
    mov observed[40], r9
    mov observed[48], rsp
    ; Legal SysV caller-saved clobbers, unsafe for an uncontained Win64 caller.
    mov rdi, 0d1h
    mov rsi, 0d2h
    pxor xmm6, xmm6
    mov rax, 1234h
    ret
CaptureGuest endp

CaptureStart proc
    mov observed[0], rdi
    mov observed[8], rsi
    mov observed[16], rdx
    mov observed[24], rcx
    mov observed[32], r8
    mov observed[40], r9
    mov observed[48], rsp
    mov observed[200], rax
    mov observed[208], rbp
    mov observed[216], r10
    mov observed[224], r11
    mov observed[232], r12
    mov observed[240], r13
    mov observed[248], r14
    mov observed[256], r15
    mov observed[264], rbx
    ; Invoke the supplied atexit stub. It returns; it is not a lifecycle exit.
    sub rsp, 8
    call rsi
    add rsp, 8
    mov qword ptr observed[272], 1
    ; Synthetic test-only escape. This does NOT test kernel teardown/longjmp.
    mov rsp, saved_wrapper_rsp
    RESTORE_HOST
CaptureStart endp

; Exercise the real guest->host assembly, with an injected HLE endpoint.
RunHleFixture proc
    SAVE_HOST
    mov rbx, 31h
    mov rbp, 32h
    mov r12, 33h
    mov r13, 34h
    mov r14, 35h
    mov r15, 36h
    SNAPSHOT_NONVOL 96
    mov rdi, 11h
    mov rsi, 22h
    mov rdx, 33h
    mov rcx, 44h
    mov r8, 55h
    mov r9, 66h
    mov r10, 77h
    movups xmm0, xmmword ptr xmm_input[0]
    movups xmm1, xmmword ptr xmm_input[16]
    movups xmm2, xmmword ptr xmm_input[32]
    movups xmm3, xmmword ptr xmm_input[48]
    movups xmm4, xmmword ptr xmm_input[64]
    movups xmm5, xmmword ptr xmm_input[80]
    movups xmm6, xmmword ptr xmm_input[96]
    movups xmm7, xmmword ptr xmm_input[112]
    lea rax, [rsp-8]
    mov observed[280], rax
    lea rax, hle_return
    mov observed[288], rax
    call HleCommonDispatcher
hle_return:
    mov observed[56], rax
    SNAPSHOT_NONVOL 144
    movups xmmword ptr observed[304], xmm0
    movups xmmword ptr observed[320], xmm1
    movups xmmword ptr observed[336], xmm2
    movups xmmword ptr observed[352], xmm3
    movups xmmword ptr observed[368], xmm4
    movups xmmword ptr observed[384], xmm5
    movups xmmword ptr observed[400], xmm6
    movups xmmword ptr observed[416], xmm7
    RESTORE_HOST
RunHleFixture endp
end
