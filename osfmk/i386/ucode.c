/*
 *  ucode.c
 *
 *  Microcode updater interface sysctl
 */

#include <kern/locks.h>
#include <i386/ucode.h>
#include <sys/errno.h>
#include <i386/proc_reg.h>
#include <i386/cpuid.h>
#include <vm/vm_kern.h>
#include <i386/mp.h>			// mp_broadcast
#include <machine/cpu_number.h> // cpu_number
#include <pexpert/pexpert.h>  // boot-args

#define IA32_BIOS_UPDT_TRIG (0x79) /* microcode update trigger MSR */

struct intel_ucupdate *global_update = NULL;

/* Exceute the actual update! */
static void
update_microcode(void)
{
	/* SDM Example 9-8 code shows that we load the
	 * address of the UpdateData within the microcode blob,
	 * not the address of the header.
	 */
	wrmsr64(IA32_BIOS_UPDT_TRIG, (uint64_t)(uintptr_t)&global_update->data);
}

/* locks */
static lck_grp_attr_t *ucode_slock_grp_attr = NULL;
static lck_grp_t *ucode_slock_grp = NULL;
static lck_attr_t *ucode_slock_attr = NULL;
static lck_spin_t *ucode_slock = NULL;

static kern_return_t
register_locks(void)
{
	/* already allocated? */
	if (ucode_slock_grp_attr && ucode_slock_grp && ucode_slock_attr && ucode_slock)
		return KERN_SUCCESS;

	/* allocate lock group attribute and group */
	if (!(ucode_slock_grp_attr = lck_grp_attr_alloc_init()))
		goto nomem_out;

	lck_grp_attr_setstat(ucode_slock_grp_attr);

	if (!(ucode_slock_grp = lck_grp_alloc_init("uccode_lock", ucode_slock_grp_attr)))
		goto nomem_out;

	/* Allocate lock attribute */
	if (!(ucode_slock_attr = lck_attr_alloc_init()))
		goto nomem_out;

	/* Allocate the spin lock */
	/* We keep one global spin-lock. We could have one per update
	 * request... but srsly, why would you update microcode like that?
	 */
	if (!(ucode_slock = lck_spin_alloc_init(ucode_slock_grp, ucode_slock_attr)))
		goto nomem_out;

	return KERN_SUCCESS;

nomem_out:
	/* clean up */
	if (ucode_slock)
		lck_spin_free(ucode_slock, ucode_slock_grp);
	if (ucode_slock_attr)
		lck_attr_free(ucode_slock_attr);
	if (ucode_slock_grp)
		lck_grp_free(ucode_slock_grp);
	if (ucode_slock_grp_attr)
		lck_grp_attr_free(ucode_slock_grp_attr);

	return KERN_NO_SPACE;
}

/* Copy in an update */
static int
copyin_update(uint64_t inaddr)
{
	struct intel_ucupdate update_header;
	struct intel_ucupdate *update;
	vm_size_t size;
	kern_return_t ret;
	int error;

	/* Copy in enough header to peek at the size */
	error = copyin((user_addr_t)inaddr, (void *)&update_header, sizeof(update_header));
	if (error)
		return error;

	/* Get the actual, alleged size */
	size = update_header.total_size;

	/* huge bogus piece of data that somehow made it through? */
	if (size >= 1024 * 1024)
		return ENOMEM;

	/* Old microcodes? */
	if (size == 0)
		size = 2048; /* default update size; see SDM */

	/*
	 * create the buffer for the update
	 * It need only be aligned to 16-bytes, according to the SDM.
	 * This also wires it down
	 */
	ret = kmem_alloc_kobject(kernel_map, (vm_offset_t *)&update, size);
	if (ret != KERN_SUCCESS)
		return ENOMEM;

	/* Copy it in */
	error = copyin((user_addr_t)inaddr, (void*)update, size);
	if (error) {
		kmem_free(kernel_map, (vm_offset_t)update, size);
		return error;
	}

	global_update = update;
	return 0;
}

/*
 * This is called once by every CPU on a wake from sleep/hibernate
 * and is meant to re-apply a microcode update that got lost
 * by sleeping.
 */
void
ucode_update_wake()
{
	if (global_update) {
		kprintf("ucode: Re-applying update after wake (CPU #%d)\n", cpu_number());
		update_microcode();
#ifdef DEBUG
	} else {
		kprintf("ucode: No update to apply (CPU #%d)\n", cpu_number());
#endif
	}
}

static void
cpu_update(__unused void *arg)
{
	/* grab the lock */
	lck_spin_lock(ucode_slock);

	/* execute the update */
	update_microcode();

	/* release the lock */
	lck_spin_unlock(ucode_slock);
}

/* Farm an update out to all CPUs */
static void
xcpu_update(void)
{
	if (register_locks() != KERN_SUCCESS)
		return;

	/* Get all CPUs to perform the update */
	mp_broadcast(cpu_update, NULL);

	/* Update the cpuid info */
	cpuid_set_info();

}

/*
 * sysctl function
 *
 */
int
ucode_interface(uint64_t addr)
{
	int error;
	char arg[16]; 

	if (PE_parse_boot_argn("-x", arg, sizeof (arg))) {
		printf("ucode: no updates in safe mode\n");
		return EPERM;
	}

#if !DEBUG
	/*
	 * Userland may only call this once per boot. Anything else
	 * would not make sense (all updates are cumulative), and also
	 * leak memory, because we don't free previous updates.
	 */
	if (global_update)
		return EPERM;
#endif

	/* Get the whole microcode */
	error = copyin_update(addr);

	if (error)
		return error;

	/* Farm out the updates */
	xcpu_update();

	return 0;
}
