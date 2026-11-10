bits 64
extern isr_handler

section .text

; For exceptions where the CPU pushes no error code, so push a fake 0 to make the frame look the same either way, then the vector, then jump to the shared path
%macro ISR_NOERR 1
isr_stub_%1:
    push qword 0
    push qword %1
    jmp isr_common
%endmacro

; For exceptions where the CPU already pushed an error code, so all that's left is the vector
%macro ISR_ERR 1
isr_stub_%1:
    push qword %1
    jmp isr_common
%endmacro

; One stub per vector. The CPU decides which ones get an error code, not us, 8, 10-14, 17, 21, 29 and 30 do and the rest don't, and getting it wrong shifts the whole frame by 8 bytes and you crash in a brand new fucked up way
ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR   8
ISR_NOERR 9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR   17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_ERR   21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_ERR   29
ISR_ERR   30
ISR_NOERR 31

; 32-47 are the PIC IRQs and none of them push an error code
%assign v 32
%rep 16
    ISR_NOERR v
%assign v v+1
%endrep

; Shared path for everything. swapgs if we came from ring 3, save every register, call the C handler with a pointer to the saved frame, restore, drop the vector and error code, swapgs back if we're returning to ring 3, iretq
isr_common:
    ; CS is at rsp+24 (past the vector, error code and RIP) and its low two bits are the privilege level, so nonzero means we interrupted user code and GS still points at user land
    test qword [rsp + 24], 3
    jz .from_kernel
    swapgs
.from_kernel:
    ; Every general register, in the order struct regs expects, because the C handler gets a pointer to this exact pile
    push rax
    push rcx
    push rdx
    push rbx
    push rbp
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
    mov rdi, rsp
    ; The ABI wants the direction flag clear on function entry and user code is allowed to leave it set, so clear it before calling any C
    cld
    call isr_handler
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdi
    pop rsi
    pop rbp
    pop rbx
    pop rdx
    pop rcx
    pop rax
    ; Throw away the vector and error code
    add rsp, 16
    ; Same privilege check on the way out, CS is at rsp+8 now that the vector and error code are gone
    test qword [rsp + 8], 3
    jz .to_kernel
    swapgs
.to_kernel:
    iretq

section .rodata
global isr_stub_table
; All 48 stub addresses, idt_init reads this to fill in the IDT
isr_stub_table:
%assign i 0
%rep 48
    dq isr_stub_%+i
%assign i i+1
%endrep
