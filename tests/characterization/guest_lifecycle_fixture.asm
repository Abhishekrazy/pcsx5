; Original synthetic guest snippets and a host ABI sentinel wrapper.
extern CallCaptured:proc
extern FixtureExit:proc
extern lifecycle_gpr_before:qword
extern lifecycle_gpr_after:qword
extern lifecycle_xmm_before:qword
extern lifecycle_xmm_after:qword
extern lifecycle_rsp_before:qword
extern lifecycle_rsp_after:qword
public LifecycleFaultStore
public LifecycleProtectedFaultStore
extern lifecycle_fault_target:qword
extern lifecycle_syscall_result:qword
.code
RunCapturedProbe proc
    push rbx
    push rbp
    push rdi
    push rsi
    push r12
    push r13
    push r14
    push r15
    sub rsp, 216
    mov [rsp+192], rcx
    mov [rsp+200], rdx
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
    mov rbx, 11h
    mov rbp, 22h
    mov rdi, 33h
    mov rsi, 44h
    mov r12, 55h
    mov r13, 66h
    mov r14, 77h
    mov r15, 88h
    mov lifecycle_gpr_before[0], rbx
    mov lifecycle_gpr_before[8], rbp
    mov lifecycle_gpr_before[16], rdi
    mov lifecycle_gpr_before[24], rsi
    mov lifecycle_gpr_before[32], r12
    mov lifecycle_gpr_before[40], r13
    mov lifecycle_gpr_before[48], r14
    mov lifecycle_gpr_before[56], r15
    movups xmm6, xmmword ptr lifecycle_xmm_before[0]
    movups xmm7, xmmword ptr lifecycle_xmm_before[16]
    movups xmm8, xmmword ptr lifecycle_xmm_before[32]
    movups xmm9, xmmword ptr lifecycle_xmm_before[48]
    movups xmm10, xmmword ptr lifecycle_xmm_before[64]
    movups xmm11, xmmword ptr lifecycle_xmm_before[80]
    movups xmm12, xmmword ptr lifecycle_xmm_before[96]
    movups xmm13, xmmword ptr lifecycle_xmm_before[112]
    movups xmm14, xmmword ptr lifecycle_xmm_before[128]
    movups xmm15, xmmword ptr lifecycle_xmm_before[144]
    mov rcx, [rsp+192]
    mov rdx, [rsp+200]
    mov lifecycle_rsp_before, rsp
    call CallCaptured
    mov lifecycle_rsp_after, rsp
    mov lifecycle_gpr_after[0], rbx
    mov lifecycle_gpr_after[8], rbp
    mov lifecycle_gpr_after[16], rdi
    mov lifecycle_gpr_after[24], rsi
    mov lifecycle_gpr_after[32], r12
    mov lifecycle_gpr_after[40], r13
    mov lifecycle_gpr_after[48], r14
    mov lifecycle_gpr_after[56], r15
    movups xmmword ptr lifecycle_xmm_after[0], xmm6
    movups xmmword ptr lifecycle_xmm_after[16], xmm7
    movups xmmword ptr lifecycle_xmm_after[32], xmm8
    movups xmmword ptr lifecycle_xmm_after[48], xmm9
    movups xmmword ptr lifecycle_xmm_after[64], xmm10
    movups xmmword ptr lifecycle_xmm_after[80], xmm11
    movups xmmword ptr lifecycle_xmm_after[96], xmm12
    movups xmmword ptr lifecycle_xmm_after[112], xmm13
    movups xmmword ptr lifecycle_xmm_after[128], xmm14
    movups xmmword ptr lifecycle_xmm_after[144], xmm15
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
    add rsp, 216
    pop r15
    pop r14
    pop r13
    pop r12
    pop rsi
    pop rdi
    pop rbp
    pop rbx
    ret
RunCapturedProbe endp

LifecycleExitGuest proc
    sub rsp, 40
    call FixtureExit
    int 3
LifecycleExitGuest endp

LifecycleFaultGuest proc
    xor rax, rax
LifecycleFaultStore::
    mov byte ptr [rax], 1
    int 3
LifecycleFaultGuest endp

LifecycleProtectedFaultGuest proc
    mov rax, lifecycle_fault_target
LifecycleProtectedFaultStore::
    mov rax, [rax]
    int 3
LifecycleProtectedFaultGuest endp

LifecycleSyscallGuest proc
    mov rdi, 11h
    mov rsi, 22h
    mov rdx, 33h
    mov r10, 44h
    mov r8, 55h
    mov r9, 66h
    mov rax, 501
    db 0cch, 090h
    mov lifecycle_syscall_result, rax
    sub rsp, 40
    call FixtureExit
    int 3
LifecycleSyscallGuest endp

LifecycleSysExitGuest proc
    mov rdi, 42
    mov rax, 1
    db 0cch, 090h
    int 3
LifecycleSysExitGuest endp
end
