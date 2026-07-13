.code

extern HleDispatch : proc
extern g_host_stack_pointer : qword

HleCommonDispatcher proc
    mov r11, [rsp] ; Get return address to guest code
    
    ; Save guest stack pointer in RAX
    mov rax, rsp
    
    ; Switch stack pointer to host stack
    mov rsp, qword ptr [g_host_stack_pointer]
    
    ; Push guest stack pointer onto host stack to preserve it for return
    push rax
    
    ; Allocate space for Windows shadow store (32 bytes) + 4 stack parameters (32 bytes) = 64 bytes
    sub rsp, 64
    
    ; Stack args mapping (SysV parameters 4, 5, 6 mapped to Windows parameters 5, 6, 7, and guest_rip to 8):
    ; rcx (4th SysV arg) -> [rsp + 32]
    ; r8  (5th SysV arg) -> [rsp + 40]
    ; r9  (6th SysV arg) -> [rsp + 48]
    ; r11 (guest_rip)    -> [rsp + 56]
    mov [rsp + 32], rcx
    mov [rsp + 40], r8
    mov [rsp + 48], r9
    mov [rsp + 56], r11
    
    ; Register args mapping:
    ; rcx = r10 (symbol_id passed from our dynamic thunk)
    ; rdx = rdi (1st SysV arg)
    ; r8  = rsi (2nd SysV arg)
    ; r9  = rdx (3rd SysV arg)
    mov rcx, r10
    mov r9, rdx
    mov rdx, rdi
    mov r8, rsi
    
    call HleDispatch
    
    ; Clean up shadow store and stack args
    add rsp, 64
    
    ; Pop guest stack pointer back into RCX
    pop rcx
    
    ; Switch stack back to guest stack
    mov rsp, rcx
    
    ret
HleCommonDispatcher endp

StartGuest proc
    ; rcx = entry_point
    ; rdx = stack_pointer
    
    ; Save host stack pointer
    mov qword ptr [g_host_stack_pointer], rsp
    
    ; Switch stack pointer to guest stack
    mov rsp, rdx
    
    ; Set up potential entry registers (System V / custom boot)
    ; rdi = stack_pointer (contains argc)
    ; rsi = 1 (argc)
    ; rdx = stack_pointer + 8 (argv pointer)
    mov rdi, rdx
    mov rsi, 1
    lea rdx, [rdi + 8]
    
    ; Clear other general registers
    xor rax, rax
    xor rbx, rbx
    xor rbp, rbp
    xor r8, r8
    xor r9, r9
    xor r10, r10
    xor r11, r11
    xor r12, r12
    xor r13, r13
    xor r14, r14
    xor r15, r15
    
    ; Jump to the guest entry point
    jmp rcx
StartGuest endp

; InvokeGuestFunction - Call a guest function from within an HLE handler.
; Uses Windows calling convention as input, translates to System V ABI for the guest.
;
; Windows ABI:  rcx = guest_func_addr
;               rdx = rdi_arg (1st SysV arg)
;               r8  = rsi_arg (2nd SysV arg)
;               r9  = rdx_arg (3rd SysV arg)
; Returns:      rax = guest return value
;
; IMPORTANT: Updates g_host_stack_pointer to this frame so that nested HLE calls
;            from within the guest function correctly use OUR stack as the host base.
InvokeGuestFunction proc
    ; Save all non-volatile Windows ABI registers
    push rbx
    push rbp
    push rsi
    push rdi
    push r12
    push r13
    push r14
    push r15
    sub  rsp, 40        ; 32-byte shadow + 8 bytes for 16-byte alignment

    ; Stash arguments in non-volatile regs
    mov  rbx, rcx       ; guest function address
    mov  r12, rdx       ; SysV rdi (arg1)
    mov  r13, r8        ; SysV rsi (arg2)
    mov  r14, r9        ; SysV rdx (arg3)

    ; Update g_host_stack_pointer so nested HLE calls in the guest function
    ; correctly restore back to THIS stack frame rather than the original host frame.
    mov  qword ptr [g_host_stack_pointer], rsp

    ; Set up System V calling convention registers
    mov  rdi, r12
    mov  rsi, r13
    mov  rdx, r14
    xor  rax, rax
    xor  rcx, rcx
    xor  r8,  r8
    xor  r9,  r9

    ; Call the guest function (it is mapped as executable host virtual memory)
    call rbx

    ; rax already holds the guest return value

    add  rsp, 40
    pop  r15
    pop  r14
    pop  r13
    pop  r12
    pop  rdi
    pop  rsi
    pop  rbp
    pop  rbx
    ret
InvokeGuestFunction endp

end
