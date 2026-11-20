// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#if defined(__x86_64__)
asm(".att_syntax prefix");
#endif

#if defined(__x86_64__)

asm(R"(
.global _runPthreadOnAnotherStack
_runPthreadOnAnotherStack:
  movq %rsi, %r11
  leaq .Lstart_guest_pthread(%rip), %rsi
.global _runOnAnotherStack
_runOnAnotherStack:
  pushq %r12
  pushq %r13
)");

#ifdef _WIN32

asm(R"(
  pushq %r14
  pushq %r15

  # save stack check registers
  movq %gs:0x08, %r14 # teb->stack_bottom
  movq %gs:0x10, %r15 # teb->stack_top

  # disable stack checks
  movq $-1, %rcx
  movq %rcx, %gs:0x08 # teb->stack_bottom = -1
  xorq %rcx, %rcx 
  movq %rcx, %gs:0x10 # teb->stack_top = 0
)");

#endif

asm(R"(
  movq %rsp, %r12
  movq %rbp, %r13
  movq %rdx, %rsp
  movq %rdx, %rbp
  callq *%rsi
  movq %r13, %rbp
  movq %r12, %rsp
)");

#ifdef _WIN32

asm(R"(
  # restore stack check registers
  movq %r14, %gs:0x08 # teb->stack_bottom
  movq %r15, %gs:0x10 # teb->stack_top
  popq %r15
  popq %r14
)");

#endif

asm(R"(
  popq %r13
  popq %r12
  ret

# The firmware clears these registers just before invoking a fresh pthread's start routine.
# Keep the stack-switch bookkeeping and host callee-saved registers across the guest call.
.Lstart_guest_pthread:
  pushq %rbx
  pushq %r12
  pushq %r13
  pushq %r14
  pushq %r15
  subq $16, %rsp
  movq %r11, (%rsp)

  pxor %mm0, %mm0
  pxor %mm1, %mm1
  pxor %mm2, %mm2
  pxor %mm3, %mm3
  pxor %mm4, %mm4
  pxor %mm5, %mm5
  pxor %mm6, %mm6
  pxor %mm7, %mm7
  emms
  vzeroall
  xorl %r15d, %r15d
  xorl %r14d, %r14d
  xorl %r13d, %r13d
  xorl %r12d, %r12d
  xorl %r11d, %r11d
  xorl %r10d, %r10d
  xorl %r9d, %r9d
  xorl %r8d, %r8d
  xorl %esi, %esi
  xorl %edx, %edx
  xorl %ecx, %ecx
  xorl %ebx, %ebx
  xorl %eax, %eax
  callq *(%rsp)

  addq $16, %rsp
  popq %r15
  popq %r14
  popq %r13
  popq %r12
  popq %rbx
  ret
)");

#elif defined(__arm64__) || defined(__aarch64__)

asm(R"(
.global _runOnAnotherStack
_runOnAnotherStack:
  // Save frame pointer (x29), link register (x30), and callee-saved registers x19, x20
  stp x29, x30, [sp, #-32]!
  mov x29, sp
  stp x19, x20, [sp, #16]

  // Save the current stack pointer into x19 (callee-saved)
  mov x19, sp
  // Save the current frame pointer into x20 (callee-saved)
  mov x20, x29

  // Align the target stack pointer (x2) to 16 bytes (ARM64 constraint)
  and x2, x2, #-16

  // Switch stack pointer to the new stack (x2)
  mov sp, x2
  // Switch frame pointer to the new stack (x2)
  mov x29, x2

  // Call the function (target address is in x1)
  // Note: the argument is already in x0!
  blr x1

  // Restore the old stack pointer and frame pointer
  mov sp, x19
  mov x29, x20

  // Restore saved registers and return
  ldp x19, x20, [sp, #16]
  ldp x29, x30, [sp], #32
  ret

)");

#endif
