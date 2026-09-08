// Test-only AAPCS64 probe. Preserve caller state, seed callee-saved registers,
// snapshot before/after the callback, then restore the original caller state.
.text
#ifdef __APPLE__
#define pcsx5_arm64_abi_probe _pcsx5_arm64_abi_probe
#define pcsx5_arm64_abi_corrupt _pcsx5_arm64_abi_corrupt
#endif
.global pcsx5_arm64_abi_probe
#ifndef __APPLE__
.type pcsx5_arm64_abi_probe,%function
#endif
.macro snapshot
    stp x19,x20,[x9,#0]
    stp x21,x22,[x9,#16]
    stp x23,x24,[x9,#32]
    stp x25,x26,[x9,#48]
    stp x27,x28,[x9,#64]
    str x29,[x9,#80]
    mov x10,sp
    str x10,[x9,#88]
    stp d8,d9,[x9,#96]
    stp d10,d11,[x9,#112]
    stp d12,d13,[x9,#128]
    stp d14,d15,[x9,#144]
    mrs x10,fpcr
    str x10,[x9,#160]
.endm
pcsx5_arm64_abi_probe:
    sub sp,sp,#208
    stp x19,x20,[sp,#0]
    stp x21,x22,[sp,#16]
    stp x23,x24,[sp,#32]
    stp x25,x26,[sp,#48]
    stp x27,x28,[sp,#64]
    stp x29,x30,[sp,#80]
    stp d8,d9,[sp,#96]
    stp d10,d11,[sp,#112]
    stp d12,d13,[sp,#128]
    stp d14,d15,[sp,#144]
    stp x0,x1,[sp,#160]
    stp x2,x3,[sp,#176]
    mrs x9,fpcr
    str x9,[sp,#192]
    mov x19,#19
    mov x20,#20
    mov x21,#21
    mov x22,#22
    mov x23,#23
    mov x24,#24
    mov x25,#25
    mov x26,#26
    mov x27,#27
    mov x28,#28
    mov x29,#29
    fmov d8,x19
    fmov d9,x20
    fmov d10,x21
    fmov d11,x22
    fmov d12,x23
    fmov d13,x24
    fmov d14,x25
    fmov d15,x26
    ldr x9,[sp,#176]
    snapshot
    ldp x9,x0,[sp,#160]
    blr x9
    ldr x9,[sp,#184]
    snapshot
    ldr x9,[sp,#192]
    msr fpcr,x9
    ldp d8,d9,[sp,#96]
    ldp d10,d11,[sp,#112]
    ldp d12,d13,[sp,#128]
    ldp d14,d15,[sp,#144]
    ldp x19,x20,[sp,#0]
    ldp x21,x22,[sp,#16]
    ldp x23,x24,[sp,#32]
    ldp x25,x26,[sp,#48]
    ldp x27,x28,[sp,#64]
    ldp x29,x30,[sp,#80]
    add sp,sp,#208
    ret
#ifndef __APPLE__
.size pcsx5_arm64_abi_probe,.-pcsx5_arm64_abi_probe
#endif
.global pcsx5_arm64_abi_corrupt
#ifndef __APPLE__
.type pcsx5_arm64_abi_corrupt,%function
#endif
pcsx5_arm64_abi_corrupt:
    mov x19,#99
    fmov d8,x19
    ret
#ifndef __APPLE__
.size pcsx5_arm64_abi_corrupt,.-pcsx5_arm64_abi_corrupt
.section .note.GNU-stack,"",%progbits
#endif
