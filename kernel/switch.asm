bits 64
section .text

; ctx_switch(&old_sp, new_sp) is the entire context switch. Push the callee-saved registers on the old stack, save the stack pointer, load the new one, pop ITS registers, and ret into wherever that thread left off. Only callee-saved ones get saved because the compiler already assumes a call trashes the rest. 15 instructions and it's the reason this kernel can do two things at once
global ctx_switch
ctx_switch:
    push rbp
    push rbx
    push r12
    push r13
    push r14
    push r15
    mov  [rdi], rsp
    mov  rsp, rsi
    pop  r15
    pop  r14
    pop  r13
    pop  r12
    pop  rbx
    pop  rbp
    ret
