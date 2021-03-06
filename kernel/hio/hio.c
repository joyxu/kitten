#include <lwk/unistd.h>
#include <lwk/kfs.h>
#include <lwk/hio.h>
#include <lwk/aspace.h>
#include <lwk/waitq.h>
#include <lwk/spinlock.h>
#include <lwk/spinlock.h>
#include <lwk/xpmem/xpmem.h>
#include <lwk/smp.h>

#include <arch/vsyscall.h>
#include <arch/atomic.h>
#include <arch/bug.h>
#include <arch-generic/fcntl.h>

#define MAX_OUTSTANDING_SYSCALLS 128

extern struct hio_implementation hio_impl;

static atomic_t hio_calls_pending = ATOMIC_INIT(0);

typedef enum {
	HIO_IDLE,
	HIO_PENDING,
	HIO_PROCESSING,
	HIO_COMPLETE,
} hio_syscall_state_t;

typedef struct {
	hio_syscall_state_t state;
	hio_syscall_t     * syscall;
	waitq_t		    waitq;
} hio_syscall_request_t;

typedef struct {
	hio_syscall_request_t entries[MAX_OUTSTANDING_SYSCALLS];
	spinlock_t            lock;
	uint32_t	      offset;
} hio_syscalls_t;

static hio_syscalls_t syscall_ring;


static inline uint32_t
next_offset(uint32_t off)
{
	return (off + 1) % MAX_OUTSTANDING_SYSCALLS;
}

static inline uint32_t
prev_offset(uint32_t off)
{
	return (off == 0) ? MAX_OUTSTANDING_SYSCALLS - 1 : off - 1;
}

static int
enqueue_syscall(hio_syscall_t * syscall)
{
	unsigned long flags = 0;
	int start, ret = -EBUSY;

	spin_lock_irqsave(&(syscall_ring.lock), flags);

	start = syscall_ring.offset;
	do {
		hio_syscall_request_t * entry = &(syscall_ring.entries[syscall_ring.offset]);

		if (entry->state == HIO_IDLE) {
			entry->state     = HIO_PENDING;
			entry->syscall   = syscall;
			syscall->uniq_id = syscall_ring.offset;
			ret              = 0;
			atomic_inc(&hio_calls_pending);
			break;
		}

		syscall_ring.offset = next_offset(syscall_ring.offset);
	} while (syscall_ring.offset != start);

	spin_unlock_irqrestore(&(syscall_ring.lock), flags);

	return ret;
}

static int
dequeue_syscall(hio_syscall_t ** syscall)
{
	unsigned long flags = 0;
	int start, ret = -ENOENT;

	spin_lock_irqsave(&(syscall_ring.lock), flags);

	start = syscall_ring.offset;
	do {
		hio_syscall_request_t * entry = &(syscall_ring.entries[syscall_ring.offset]);
		if (entry->state == HIO_PENDING) {
			entry->state   = HIO_PROCESSING;
			*syscall       = entry->syscall;
			ret            = 0;
			atomic_dec(&hio_calls_pending);
			break;
		}
		syscall_ring.offset = prev_offset(syscall_ring.offset);
	} while (syscall_ring.offset != start);

	spin_unlock_irqrestore(&(syscall_ring.lock), flags);
	
	return ret;
}

void
hio_cancel_syscall(hio_syscall_t * syscall)
{
	unsigned long flags;
	hio_syscall_request_t * entry;

	BUG_ON((syscall->uniq_id < 0) || (syscall->uniq_id > MAX_OUTSTANDING_SYSCALLS));

	spin_lock_irqsave(&(syscall_ring.lock), flags);

	entry = &(syscall_ring.entries[syscall->uniq_id]);
	entry->state = HIO_IDLE;

	spin_unlock_irqrestore(&(syscall_ring.lock), flags);
}

void
hio_return_syscall(hio_syscall_t * syscall)
{
	unsigned long flags;
	hio_syscall_request_t * entry;

	BUG_ON((syscall->uniq_id < 0) || (syscall->uniq_id > MAX_OUTSTANDING_SYSCALLS));

	spin_lock_irqsave(&(syscall_ring.lock), flags);

	entry = &(syscall_ring.entries[syscall->uniq_id]);

	/* It could have been canceled by the issuer (e.g, they took a signal) */
	if (entry->state != HIO_PROCESSING) {
	    spin_unlock_irqrestore(&(syscall_ring.lock), flags);
	    return;
	}

	/* copy in ret_val and hio segs */
	memcpy(entry->syscall->segs, syscall->segs, syscall->segc * sizeof(hio_segment_t));
	entry->syscall->segc    = syscall->segc;
	entry->syscall->ret_val = syscall->ret_val;
	entry->state            = HIO_COMPLETE;

	spin_unlock_irqrestore(&(syscall_ring.lock), flags);

	mb();
	waitq_wakeup(&(entry->waitq));
}

static int
hio_wait_syscall(hio_syscall_t * syscall,
		 uintptr_t     * ret_val)
{
	hio_syscall_request_t * entry;
	int status;

	BUG_ON((syscall->uniq_id < 0) || (syscall->uniq_id > MAX_OUTSTANDING_SYSCALLS));

	entry = &(syscall_ring.entries[syscall->uniq_id]);
	
	status = wait_event_interruptible(
		entry->waitq,
		(entry->state == HIO_COMPLETE)
	);
	mb();

	if (entry->state == HIO_COMPLETE) {
		*ret_val = entry->syscall->ret_val;
		hio_cancel_syscall(syscall);
	} else 
		*ret_val = status;

	return status;
}


int
hio_issue_syscall(hio_syscall_t * syscall)
{
	int status;

	/* Enqueue the call */
	status = enqueue_syscall(syscall);
	if (status != 0) {
		printk(KERN_ERR "Failed to enqueue HIO syscall (err:%d)\n", status);
		return status;
	}

	/* Send the notification */
	status = hio_impl.notify_new_syscall();
	if (status != 0) {
		printk(KERN_ERR "Failed to issue HIO syscall notification (err:%d)\n", status);
		hio_cancel_syscall(syscall);
		return status;
	}

	return 0;
}

uintptr_t
hio_format_and_exec_syscall(uint32_t syscall_nr,
	  	            uint32_t argc,
		            ...)
{
	uint32_t  i;
	va_list   argp;
	int       status;
	uintptr_t ret_val;
	hio_syscall_t * syscall;
	struct file   * xpmem_f;

	//printk("%d cpu %d: in syscall %d\n", current->id, this_cpu, syscall_nr);

	if (argc > HIO_MAX_ARGC)
		return -EINVAL;

	syscall = kmem_alloc(sizeof(hio_syscall_t));
	if (syscall == NULL)
		return -ENOMEM;

	syscall->aspace_id  = current->aspace->id;
	syscall->thread_id  = current->id;
	syscall->rank_id    = current->rank;
	syscall->syscall_nr = syscall_nr;
	syscall->argc       = argc;

	va_start(argp, argc);
	for (i = 0; i < argc; i++)
		syscall->args[i] = va_arg(argp, uintptr_t);
	va_end(argp);

	/* Send syscall */
	status = hio_issue_syscall(syscall);
	if (status) {
		kmem_free(syscall);
		return status;
	}

	/* Wait for response */
	status  = hio_wait_syscall(syscall, &ret_val);

	if (status) {
		kmem_free(syscall);
		return status;
	}

	/* Open /dev/xpmem, so the xpmem driver initializes this process */
	if (kfs_open_path("/dev/xpmem", O_RDWR, 0666, &xpmem_f)) {
		printk(KERN_ERR "Could not open /dev/xpmem\n");
		kmem_free(syscall);
		return -ENODEV;
	}

	/* Attach each of the xpmem segments */
	for (i = 0; i < syscall->segc; i++) {
		xpmem_apid_t apid;
		vaddr_t      target_vaddr, at_vaddr;

		/* Get it */ 
		status = xpmem_get(
			syscall->segs[i].segid,
			XPMEM_RDWR,
			XPMEM_GLOBAL_MODE,
			NULL,
			&apid);

		if (status) {
			printk(KERN_ERR "Failed to get HIO segid: %lli (status: %d)\n", 
				    syscall->segs[i].segid, status);
			ret_val = (uintptr_t)status;
			goto out;
		}

		target_vaddr = (vaddr_t)syscall->segs[i].target_vaddr;

		/* Attach it */
		status = xpmem_attach(
			apid,
			0,
			syscall->segs[i].size,
			target_vaddr,
			0,
			&at_vaddr);

		if (status) {
			printk(KERN_ERR "Failed to attach to HIO apid (status: %d)\n", status);
			xpmem_release(apid);
			ret_val = (uintptr_t)status;
			goto out;
		}

		BUG_ON(target_vaddr != at_vaddr);
	}

out:
	/* This is going to be an issue if the process is actually using xpmem/xemem at user level ... */
	kfs_close(xpmem_f);
	kmem_free(syscall);
	//printk("%d cpu %d: out syscall %d, ret_val = %lu (0x%lx)\n", current->id, this_cpu, syscall_nr, (unsigned long)ret_val, (unsigned long)ret_val);
	return ret_val;
}

uint32_t
hio_get_num_pending_syscalls(void)
{
	return atomic_read(&hio_calls_pending);
}

int
hio_get_pending_syscall(hio_syscall_t ** pending_syscall)
{
	return dequeue_syscall(pending_syscall);
}

static void
init_syscall_ring_entry(hio_syscall_request_t * entry)
{
	entry->state = HIO_IDLE;
	waitq_init(&(entry->waitq));
}

static int
syscall_init(void)
{
	uint32_t i;

	memset(&(syscall_ring.entries), 0, sizeof(hio_syscall_request_t) * MAX_OUTSTANDING_SYSCALLS);
	spin_lock_init(&(syscall_ring.lock));
	syscall_ring.offset = 0;

	for (i = 0; i < MAX_OUTSTANDING_SYSCALLS; i++)
		init_syscall_ring_entry(&(syscall_ring.entries[i]));

	return 0;
}

void __init
hio_syscall_init(void)
{
	if (syscall_init() != 0) {
		printk(KERN_ERR "Failed to initialize HIO system call subsystem\n");
		return;
	}

	if (hio_impl.init() != 0) {
		printk(KERN_ERR "Failed to initialize HIO system call implementation\n");
		return;
	}

	/* Architecture-specific intialization */
	syscall_register(__NR_open, (syscall_ptr_t) hio_open);
	syscall_register(__NR_close, (syscall_ptr_t) hio_close);
	syscall_register(__NR_read, (syscall_ptr_t) hio_read);
	syscall_register(__NR_write, (syscall_ptr_t) hio_write);
	syscall_register(__NR_readv, (syscall_ptr_t) hio_readv);
	syscall_register(__NR_writev, (syscall_ptr_t) hio_writev);
	syscall_register(__NR_mmap, (syscall_ptr_t) hio_mmap);
	syscall_register(__NR_munmap, (syscall_ptr_t) hio_munmap);
	syscall_register(__NR_ioctl, (syscall_ptr_t) hio_ioctl);
	syscall_register(__NR_openat, (syscall_ptr_t) hio_openat);
	syscall_register(__NR_getdents, (syscall_ptr_t) hio_getdents);
	syscall_register(__NR_getdents64, (syscall_ptr_t) hio_getdents64);
	syscall_register(__NR_stat, (syscall_ptr_t) hio_stat);
	syscall_register(__NR_fstat, (syscall_ptr_t) hio_fstat);
	syscall_register(__NR_socket, (syscall_ptr_t) hio_socket);
	syscall_register(__NR_accept, (syscall_ptr_t) hio_accept);
	syscall_register(__NR_bind, (syscall_ptr_t) hio_bind);
	syscall_register(__NR_listen, (syscall_ptr_t) hio_listen);
	syscall_register(__NR_connect, (syscall_ptr_t) hio_connect);
	syscall_register(__NR_getsockopt, (syscall_ptr_t) hio_getsockopt);
	syscall_register(__NR_setsockopt, (syscall_ptr_t) hio_setsockopt);
	syscall_register(__NR_getsockname, (syscall_ptr_t) hio_getsockname);
	syscall_register(__NR_sendto, (syscall_ptr_t) hio_sendto);
	syscall_register(__NR_recvmsg, (syscall_ptr_t) hio_recvmsg);
	syscall_register(__NR_lseek, (syscall_ptr_t) hio_lseek);
	syscall_register(__NR_fcntl, (syscall_ptr_t) hio_fcntl);
	syscall_register(__NR_uname, (syscall_ptr_t) hio_uname);
	syscall_register(__NR_getpid, (syscall_ptr_t) hio_getpid);
	syscall_register(__NR_gettid, (syscall_ptr_t) hio_gettid);
	syscall_register(__NR_set_tid_address, (syscall_ptr_t) hio_set_tid_address);
	syscall_register(__NR_newfstatat, (syscall_ptr_t) hio_newfstatat);
	syscall_register(__NR_ftruncate, (syscall_ptr_t) hio_ftruncate);
	syscall_register(__NR_poll, (syscall_ptr_t) hio_poll);
	syscall_register(__NR_recvfrom, (syscall_ptr_t) hio_recvfrom);
	syscall_register(__NR_statfs, (syscall_ptr_t) hio_statfs);
	syscall_register(__NR_readlink, (syscall_ptr_t) hio_readlink);
	syscall_register(__NR_faccessat, (syscall_ptr_t) hio_faccessat);
	syscall_register(__NR_unlink, (syscall_ptr_t) hio_unlink);
	syscall_register(__NR_shmget, (syscall_ptr_t) hio_shmget);
	syscall_register(__NR_shmat, (syscall_ptr_t) hio_shmat);
	syscall_register(__NR_shmctl, (syscall_ptr_t) hio_shmctl);
}
