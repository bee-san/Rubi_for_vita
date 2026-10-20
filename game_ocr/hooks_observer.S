/* The table sites' observer (game_ocr/hooks.c, docs/native-hooks.md), from the removed
 * text/observer.S (c35ebf9^) with its names changed. ARM code, used for ARM and Thumb sites: taiHEN's
 * offset hook jumps here (an LDR to the PC, no link) with the game's registers as they were at the site.
 *
 * It saves r0-r12, the site's SP and LR, APSR (with GE) and FPSCR in a 72-byte frame and d0-d31 below
 * it, calls vjo_hooks_observe(slot, frame) on an 8-byte-aligned stack, and puts back every register,
 * the flags and the stack exactly as they were before it jumps to the continuation that C returned
 * (taiHEN's next hook, or the trampoline of the moved instructions), as TAI_CONTINUE would.
 * Frame: [0..51] r0-r12, [52] the site's SP, [56] its LR, [60] the continuation (C writes the site's
 * address there first, as regs[15]; the return value replaces it), [64] APSR, [68] FPSCR.
 * The instructions the hook moves are taiHEN's to relocate: core/hook_place.c accepts only sites
 * whose moved instructions need no PC (a function's entry, plain register and stack instructions). */
.syntax unified
.arch armv7-a
.fpu neon
.arm
.text
.align 2
.macro OBSERVER slot
.global vjo_hooks_observer_\slot
.type vjo_hooks_observer_\slot, %function
vjo_hooks_observer_\slot:
    sub sp, sp, #72
    stmia sp, {r0-r12}
    add r0, sp, #72
    str r0, [sp, #52]
    str lr, [sp, #56]
    mrs r0, APSR
    str r0, [sp, #64]
    vmrs r0, fpscr
    str r0, [sp, #68]
    vpush {d0-d15}
    vpush {d16-d31}
    mov r4, sp
    bic sp, sp, #7
    mov r0, #\slot
    add r1, r4, #256
    bl vjo_hooks_observe
    str r0, [r4, #316]
    mov sp, r4
    vpop {d16-d31}
    vpop {d0-d15}
    ldr r0, [sp, #68]
    vmsr fpscr, r0
    ldr r0, [sp, #64]
    msr APSR_nzcvqg, r0
    ldmia sp, {r0-r12}
    ldr lr, [sp, #56]
    add sp, sp, #60
    ldr pc, [sp], #12
.size vjo_hooks_observer_\slot, .-vjo_hooks_observer_\slot
.endm
OBSERVER 0
OBSERVER 1
OBSERVER 2
OBSERVER 3
OBSERVER 4
OBSERVER 5
OBSERVER 6
OBSERVER 7
.section .note.GNU-stack,"",%progbits
