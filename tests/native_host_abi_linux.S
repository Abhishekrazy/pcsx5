.intel_syntax noprefix
.text
.globl pcsx5_host_probe
.type pcsx5_host_probe,@function
pcsx5_host_probe:
    .cfi_startproc
    sub rsp,40
    .cfi_def_cfa_offset 48
    mov [rsp],rdi
    mov [rsp+8],rsi
    mov [rsp+16],rdx
    mov [rsp+24],rcx
    mov r10,rdx
    mov [r10],rbx
    mov [r10+8],rbp
    mov [r10+16],r12
    mov [r10+24],r13
    mov [r10+32],r14
    mov [r10+40],r15
    mov [r10+64],rsp
    stmxcsr [r10+72]
    fnstcw [r10+76]
    pushfq
    pop rax
    mov [r10+80],rax
    mov rdi,[rsp+8]
    call QWORD PTR [rsp]
    mov r10,[rsp+24]
    mov [r10],rbx
    mov [r10+8],rbp
    mov [r10+16],r12
    mov [r10+24],r13
    mov [r10+32],r14
    mov [r10+40],r15
    mov [r10+64],rsp
    stmxcsr [r10+72]
    fnstcw [r10+76]
    pushfq
    pop rax
    mov [r10+80],rax
    mov r10,[rsp+16]
    mov rbx,[r10]
    mov rbp,[r10+8]
    mov r12,[r10+16]
    mov r13,[r10+24]
    mov r14,[r10+32]
    mov r15,[r10+40]
    ldmxcsr [r10+72]
    fldcw [r10+76]
    cld
    add rsp,40
    .cfi_def_cfa_offset 8
    ret
    .cfi_endproc
.size pcsx5_host_probe,.-pcsx5_host_probe
.globl pcsx5_host_corrupt
.type pcsx5_host_corrupt,@function
pcsx5_host_corrupt:
    not rbx
    sub rsp,8
    stmxcsr [rsp]
    xor DWORD PTR [rsp],0x2000
    ldmxcsr [rsp]
    fnstcw [rsp+4]
    xor WORD PTR [rsp+4],0x400
    fldcw [rsp+4]
    add rsp,8
    std
    ret
.size pcsx5_host_corrupt,.-pcsx5_host_corrupt
.section .note.GNU-stack,"",@progbits
