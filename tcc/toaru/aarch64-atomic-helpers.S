/* aarch64-atomic-helpers.S — libatomic-style helpers the ToaruOS aarch64
 * libc.a expects from the linker.
 *
 * The ToaruOS aarch64 userland is compiled by gcc for DYNAMIC linking
 * (everything goes through ld.so and libc.so).  The static libc.a in the
 * release ramdisk references the libatomic internal helpers gcc calls for
 * __sync_lock_test_and_set() on aarch64 without LSE — upstream never
 * statically links libc.a, so the helpers normally come from libatomic.a in
 * the gcc toolchain, which we do not ship.
 *
 * Calling convention (measured from the release libc.a, atexit.o/malloc.o:
 *   mov x1, <ptr> ; mov w0, <val> ; bl __aarch64_swp4_acq
 * — the old value is returned in w0).
 *
 * Currently exactly one helper is referenced (see also tcc-toaru.patch and
 * scripts/mktcc-aarch64.sh); add more variants here if a future libc uses
 * them: __aarch64_cas*, __aarch64_ldadd*, __aarch64_swp{1,2,8}_*.
 */

	.text
	.align  2

/* uint32_t __aarch64_swp4_acq(uint32_t val (w0), uint32_t *ptr (x1))
   Swap val into *ptr and return the old value; acquire ordering. */
	.global __aarch64_swp4_acq
	.type   __aarch64_swp4_acq, %function
__aarch64_swp4_acq:
	mov	w3, w0              /* the value to store */
1:	ldaxr	w0, [x1]            /* old value, acquire */
	stlxr	w4, w3, [x1]        /* store-release; w4 = failure flag */
	cbnz	w4, 1b
	ret
	.size	__aarch64_swp4_acq, .-__aarch64_swp4_acq
