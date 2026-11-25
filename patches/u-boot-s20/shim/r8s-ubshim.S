/*
 * r8s-ubshim: what S-Boot jumps to in BOOT's kernel slot, in front of U-Boot.
 * Before any U-Boot code runs:
 *  - DECON0 HW_SW_TRIG_CONTROL, so the framebuffer refreshes;
 *  - PMU INFORM2 bits 23:16 = 0x4e, so S-Boot doesn't pick charging mode
 *    (LPM) on the next reset;
 *  - a boot counter in PMU INFORM 0x85c (0x5238nnnn), which S-Boot leaves
 *    alone across software and watchdog resets. The initramfs zeroes it once
 *    SSH is listening. Each pass through here adds one; the third pass in a
 *    row without a kernel getting that far gives up: INFORM3 = recovery and
 *    a reset, so Lineage Recovery is the last resort.
 *  - otherwise INFORM3 = normal, so a crash, panic or watchdog reset comes
 *    back here (to U-Boot and the kernel in BOOT), and the cluster0 watchdog
 *    armed at its longest timeout (0xffff ticks of 26 MHz / 256 / 128,
 *    about 82 s). U-Boot stops it when its menu opens; the kernel's
 *    s3c2410_wdt (tmr_atboot=1) takes it over.
 * Then U-Boot, 4 KiB further on, with S-Boot's x0-x3 (x0 = its FDT).
 */
#define PMU_BASE		0x15860000
#define PMU_INFORM2		0x808
#define PMU_INFORM3		0x80c
#define PMU_BOOTCNT		0x85c	/* not 0x860-0x87c: EL3 keeps per-CPU state there */
#define PMU_CL0_NONCPU_INT_EN	0x1244	/* bit 2: cluster0 WDT reset request */
#define PMU_SWRESET		0x15863a00
#define BOOTCNT_MAGIC		0x5238	/* upper half; low byte = count */
#define BOOTCNT_GIVE_UP		3
#define INFORM2_NOT_LPM		0x4e0000
#define INFORM3_NORMAL		0x12345600
#define INFORM3_RECOVERY	0x12345604
#define WDT_BASE		0x10050000	/* cluster0: WTCON, WTDAT +4, WTCNT +8 */
#define WTCON_ARM		0x1ff39	/* DBGACK mask, prescaler 0xff, /128, enable, reset */
#define WDT_COUNT		0xffff
#define WDT_COUNT_NOW		0x200	/* ~0.65 s */
#define DECON0_TRIG		0x19050070
#define DECON0_TRIG_VAL		0x1281

	.text
	.globl _start
_start:
	mov	x19, x0
	mov	x20, x1
	mov	x21, x2
	mov	x22, x3

	ldr	x9, =DECON0_TRIG
	ldr	w10, =DECON0_TRIG_VAL
	str	w10, [x9]

	ldr	x8, =PMU_BASE
	ldr	w10, [x8, #PMU_INFORM2]
	and	w10, w10, #0xff00ffff
	ldr	w11, =INFORM2_NOT_LPM
	orr	w10, w10, w11
	str	w10, [x8, #PMU_INFORM2]

	/* boot counter: anything without the magic counts as 0 */
	mov	w12, #BOOTCNT_MAGIC
	ldr	w10, [x8, #PMU_BOOTCNT]
	cmp	w12, w10, lsr #16
	b.eq	1f
	lsl	w10, w12, #16
1:	add	w10, w10, #1
	and	w11, w10, #0xff
	cmp	w11, #BOOTCNT_GIVE_UP
	b.hs	give_up
	str	w10, [x8, #PMU_BOOTCNT]

	ldr	w10, =INFORM3_NORMAL
	str	w10, [x8, #PMU_INFORM3]

	ldr	x9, =WDT_BASE
	str	wzr, [x9]
	ldr	w10, =WDT_COUNT
	str	w10, [x9, #4]
	str	w10, [x9, #8]
	ldr	w10, =WTCON_ARM
	str	w10, [x9]
	ldr	w10, [x8, #PMU_CL0_NONCPU_INT_EN]
	orr	w10, w10, #4
	str	w10, [x8, #PMU_CL0_NONCPU_INT_EN]

	dsb	sy
	isb
	mov	x0, x19
	mov	x1, x20
	mov	x2, x21
	mov	x3, x22
	b	payload

give_up:
	lsl	w10, w12, #16			/* count 0 for whatever boots next */
	str	w10, [x8, #PMU_BOOTCNT]
	ldr	w10, =INFORM3_RECOVERY
	str	w10, [x8, #PMU_INFORM3]
	/* The reset: the watchdog at ~0.65 s (proven on this board), and
	 * PMU SWRESET too, whichever comes first. */
	ldr	x9, =WDT_BASE
	str	wzr, [x9]
	mov	w10, #WDT_COUNT_NOW
	str	w10, [x9, #4]
	str	w10, [x9, #8]
	ldr	w10, =WTCON_ARM
	str	w10, [x9]
	ldr	w10, [x8, #PMU_CL0_NONCPU_INT_EN]
	orr	w10, w10, #4
	str	w10, [x8, #PMU_CL0_NONCPU_INT_EN]
	dsb	sy
	ldr	x9, =PMU_SWRESET
	mov	w10, #2
	str	w10, [x9]
	dsb	sy
2:	wfi
	b	2b

	.ltorg
	.ascii	"r8s-ubshim2"

	.balign	4096
payload:
	.incbin	UBOOT_BIN
