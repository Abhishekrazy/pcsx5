OPTION CASEMAP:NONE
PUBLIC pcsx5_host_probe
PUBLIC pcsx5_host_corrupt
.code
pcsx5_host_probe PROC FRAME
    sub rsp,72
    .allocstack 72
    .endprolog
    mov [rsp+32],rcx
    mov [rsp+40],rdx
    mov [rsp+48],r8
    mov [rsp+56],r9
    mov r10,r8
    mov [r10],rbx
    mov [r10+8],rbp
    mov [r10+16],rdi
    mov [r10+24],rsi
    mov [r10+32],r12
    mov [r10+40],r13
    mov [r10+48],r14
    mov [r10+56],r15
    mov [r10+64],rsp
    stmxcsr [r10+72]
    fnstcw [r10+76]
    pushfq
    pop rax
    mov [r10+80],rax
    movdqu [r10+96],xmm6
    movdqu [r10+112],xmm7
    movdqu [r10+128],xmm8
    movdqu [r10+144],xmm9
    movdqu [r10+160],xmm10
    movdqu [r10+176],xmm11
    movdqu [r10+192],xmm12
    movdqu [r10+208],xmm13
    movdqu [r10+224],xmm14
    movdqu [r10+240],xmm15
    mov rcx,[rsp+40]
    call QWORD PTR [rsp+32]
    mov r10,[rsp+56]
    mov [r10],rbx
    mov [r10+8],rbp
    mov [r10+16],rdi
    mov [r10+24],rsi
    mov [r10+32],r12
    mov [r10+40],r13
    mov [r10+48],r14
    mov [r10+56],r15
    mov [r10+64],rsp
    stmxcsr [r10+72]
    fnstcw [r10+76]
    pushfq
    pop rax
    mov [r10+80],rax
    movdqu [r10+96],xmm6
    movdqu [r10+112],xmm7
    movdqu [r10+128],xmm8
    movdqu [r10+144],xmm9
    movdqu [r10+160],xmm10
    movdqu [r10+176],xmm11
    movdqu [r10+192],xmm12
    movdqu [r10+208],xmm13
    movdqu [r10+224],xmm14
    movdqu [r10+240],xmm15
    mov r10,[rsp+48]
    mov rbx,[r10]
    mov rbp,[r10+8]
    mov rdi,[r10+16]
    mov rsi,[r10+24]
    mov r12,[r10+32]
    mov r13,[r10+40]
    mov r14,[r10+48]
    mov r15,[r10+56]
    movdqu xmm6,[r10+96]
    movdqu xmm7,[r10+112]
    movdqu xmm8,[r10+128]
    movdqu xmm9,[r10+144]
    movdqu xmm10,[r10+160]
    movdqu xmm11,[r10+176]
    movdqu xmm12,[r10+192]
    movdqu xmm13,[r10+208]
    movdqu xmm14,[r10+224]
    movdqu xmm15,[r10+240]
    ldmxcsr [r10+72]
    fldcw [r10+76]
    cld
    add rsp,72
    ret
pcsx5_host_probe ENDP
pcsx5_host_corrupt PROC
    not rbx
    pcmpeqd xmm0,xmm0
    pxor xmm6,xmm0
    sub rsp,8
    stmxcsr [rsp]
    xor DWORD PTR [rsp],2000h
    ldmxcsr [rsp]
    fnstcw [rsp+4]
    xor WORD PTR [rsp+4],400h
    fldcw [rsp+4]
    add rsp,8
    std
    ret
pcsx5_host_corrupt ENDP
END
