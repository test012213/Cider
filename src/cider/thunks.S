/*
 * Calling-convention bridges between macOS (System V AMD64) code and
 * Cider's Windows (Microsoft x64) implementation.
 *
 * Variadic functions can't be written in C with sysv_abi on Windows, so each
 * one gets a thunk that spills the System V argument registers into a
 * register save area, builds a System V va_list over it, and calls a normal
 * C function with (uint64_t *gprs, sysv_va *va).
 */
    .text

/* Frame layout (rsp after prologue, 16-byte aligned):
 *   0..31    shadow space for the MS x64 call
 *   32..79   rdi rsi rdx rcx r8 r9        (System V register save area start)
 *   80..207  xmm0..xmm7
 *   208..231 va_list {gp_offset, fp_offset, overflow_arg_area, reg_save_area}
 */
.macro VARTHUNK name, impl, nfixed
    .globl \name
    .p2align 4
\name:
    pushq   %rbp
    movq    %rsp, %rbp
    subq    $240, %rsp
    movq    %rdi, 32(%rsp)
    movq    %rsi, 40(%rsp)
    movq    %rdx, 48(%rsp)
    movq    %rcx, 56(%rsp)
    movq    %r8,  64(%rsp)
    movq    %r9,  72(%rsp)
    movaps  %xmm0, 80(%rsp)
    movaps  %xmm1, 96(%rsp)
    movaps  %xmm2, 112(%rsp)
    movaps  %xmm3, 128(%rsp)
    movaps  %xmm4, 144(%rsp)
    movaps  %xmm5, 160(%rsp)
    movaps  %xmm6, 176(%rsp)
    movaps  %xmm7, 192(%rsp)
    movl    $(\nfixed * 8), 208(%rsp)
    movl    $48, 212(%rsp)
    leaq    16(%rbp), %rax
    movq    %rax, 216(%rsp)
    leaq    32(%rsp), %rax
    movq    %rax, 224(%rsp)
    leaq    32(%rsp), %rcx
    leaq    208(%rsp), %rdx
    call    \impl
    leave
    ret
.endm

VARTHUNK th_printf,       vimpl_printf,       1
VARTHUNK th_fprintf,      vimpl_fprintf,      2
VARTHUNK th_sprintf,      vimpl_sprintf,      2
VARTHUNK th_snprintf,     vimpl_snprintf,     3
VARTHUNK th_asprintf,     vimpl_asprintf,     2
VARTHUNK th_dprintf,      vimpl_dprintf,      2
VARTHUNK th_sprintf_chk,  vimpl_sprintf_chk,  4
VARTHUNK th_snprintf_chk, vimpl_snprintf_chk, 5
VARTHUNK th_sscanf,       vimpl_sscanf,       2
VARTHUNK th_fscanf,       vimpl_fscanf,       2
VARTHUNK th_scanf,        vimpl_scanf,        1
VARTHUNK th_syslog,       vimpl_syslog,       2
VARTHUNK th_err,          vimpl_err,          2
VARTHUNK th_errx,         vimpl_errx,         2
VARTHUNK th_warn,         vimpl_warn,         1
VARTHUNK th_warnx,        vimpl_warnx,        1
VARTHUNK th_open,         vimpl_open,         2
VARTHUNK th_openat,       vimpl_openat,       3
VARTHUNK th_fcntl,        vimpl_fcntl,        2
VARTHUNK th_ioctl,        vimpl_ioctl,        2

/* Thread-local variable accessor (dyld's tlv_get_addr contract):
 * rdi = descriptor, returns address in rax, preserves every other register. */
    .globl cider_tlv_thunk
    .p2align 4
cider_tlv_thunk:
    pushq   %rbp
    movq    %rsp, %rbp
    pushq   %rcx
    pushq   %rdx
    pushq   %rsi
    pushq   %rdi
    pushq   %r8
    pushq   %r9
    pushq   %r10
    pushq   %r11
    subq    $128, %rsp
    movdqu  %xmm0, 32(%rsp)
    movdqu  %xmm1, 48(%rsp)
    movdqu  %xmm2, 64(%rsp)
    movdqu  %xmm3, 80(%rsp)
    movdqu  %xmm4, 96(%rsp)
    movdqu  %xmm5, 112(%rsp)
    movq    %rdi, %rcx
    call    cider_tlv_addr
    movdqu  32(%rsp), %xmm0
    movdqu  48(%rsp), %xmm1
    movdqu  64(%rsp), %xmm2
    movdqu  80(%rsp), %xmm3
    movdqu  96(%rsp), %xmm4
    movdqu  112(%rsp), %xmm5
    addq    $128, %rsp
    popq    %r11
    popq    %r10
    popq    %r9
    popq    %r8
    popq    %rdi
    popq    %rsi
    popq    %rdx
    popq    %rcx
    popq    %rbp
    ret

/* ___chkstk_darwin: probe rax bytes of stack below the caller, page by page.
 * Must preserve all registers. */
    .globl cider_chkstk_darwin
    .p2align 4
cider_chkstk_darwin:
    pushq   %rcx
    pushq   %rax
    leaq    24(%rsp), %rcx
1:
    cmpq    $0x1000, %rax
    jb      2f
    subq    $0x1000, %rcx
    testq   %rcx, (%rcx)
    subq    $0x1000, %rax
    jmp     1b
2:
    subq    %rax, %rcx
    testq   %rcx, (%rcx)
    popq    %rax
    popq    %rcx
    ret

/* void cider_jump_start(void *entry, void *sp)  -- MS ABI: rcx, rdx */
    .globl cider_jump_start
    .p2align 4
cider_jump_start:
    movq    %rdx, %rsp
    xorl    %ebp, %ebp
    jmpq    *%rcx

/* setjmp/longjmp for macOS code (System V: rdi = jmp_buf, esi = value).
 * Saves the System V callee-saved registers, stack pointer, return
 * address, MXCSR and the x87 control word (72 bytes; macOS jmp_buf is 148). */
    .globl cider_setjmp
    .p2align 4
cider_setjmp:
    movq    %rbx, 0(%rdi)
    movq    %rbp, 8(%rdi)
    movq    %r12, 16(%rdi)
    movq    %r13, 24(%rdi)
    movq    %r14, 32(%rdi)
    movq    %r15, 40(%rdi)
    leaq    8(%rsp), %rax
    movq    %rax, 48(%rdi)
    movq    (%rsp), %rax
    movq    %rax, 56(%rdi)
    stmxcsr 64(%rdi)
    fnstcw  68(%rdi)
    xorl    %eax, %eax
    ret

    .globl cider_longjmp
    .p2align 4
cider_longjmp:
    movl    %esi, %eax
    testl   %eax, %eax
    jnz     1f
    movl    $1, %eax
1:
    movq    0(%rdi), %rbx
    movq    8(%rdi), %rbp
    movq    16(%rdi), %r12
    movq    24(%rdi), %r13
    movq    32(%rdi), %r14
    movq    40(%rdi), %r15
    ldmxcsr 64(%rdi)
    fldcw   68(%rdi)
    movq    48(%rdi), %rsp
    jmpq    *56(%rdi)
