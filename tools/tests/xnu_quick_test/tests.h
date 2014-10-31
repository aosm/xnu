#ifndef _TESTS_H_
#define	_TESTS_H_

#ifndef CONFORMANCE_TESTS_IN_XNU
#define CONFORMANCE_TESTS_IN_XNU       0
#endif
#ifndef TEST_SYSTEM_CALLS
#define TEST_SYSTEM_CALLS              0
#endif

#include <errno.h>
#include <fcntl.h>
#include <signal.h>		/* Install signal handlers*/
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>
#include <assert.h>
#include <stdarg.h>		/* Used to support printf() in misc.c */
#include <mach/machine.h>	/* Used to determine host properties */
#include <mach/vm_inherit.h>
#include <sys/acct.h>
#include <sys/aio.h>
#include <sys/attr.h>
#include <sys/dirent.h>
#include <sys/disk.h>
#include <sys/uio.h>
#include <sys/kauth.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/param.h>
#include <sys/ptrace.h>
#include <sys/quota.h>
#include <sys/resource.h>
#include <sys/select.h>
#include <sys/signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysctl.h>		/* Used to determine host properties */
#include <sys/syslimits.h>
#include <sys/time.h>
#include <sys/ttycom.h>
#include <sys/types.h>
#include <sys/ucred.h>
#include <sys/vnode.h>
#include <sys/wait.h>
#include <TargetConditionals.h> /* for TARGET_OS_EMBEDDED */

#define MY_BUFFER_SIZE (1024 * 10)
#define ARM 	100  /* I am not sure if the value really matters? */
#define INTEL	38947			/* 
					 * Random values used by execve tests to 
					 * determine architecture of machine.
					 */

#define FILE_NOTME "/private/tmp/notme"		/* file in /private/tmp not owned by the current user */
#define FILE_ME "/private/tmp/me"		/* file in /private/tmp owned by the current user */

typedef int (*test_rtn_t)(void *);

int access_chmod_fchmod_test( void * the_argp );
int acct_test( void * the_argp );
int aio_tests( void * the_argp );
int bsd_shm_tests( void * the_argp );
int chdir_fchdir_test( void * the_argp );
int chflags_fchflags_test( void * the_argp );
int chroot_test( void * the_argp );
int chown_fchown_lchown_lstat_symlink_test( void * the_argp );
int create_file_with_name( char *the_pathp, char *the_namep, int remove_existing );
int create_random_name( char *the_pathp, int do_open );
int directory_tests( void * the_argp );
int do_execve_test(char * path, char * argv[], void * envpi, int killwait);
int do_spawn_test(int arch, int shouldfail);
int dup_test( void * the_argp );
int exchangedata_test( void * the_argp );
int execve_kill_vfork_test( void * the_argp );
int fcntl_test( void * the_argp );
int fork_wait4_exit_test( void * the_argp );
int fs_stat_tests( void * the_argp );
int get_architecture(void);				/* Intel or PPC */
int get_bits(void);					/* 64 or 32 */
int getlogin_setlogin_test( void * the_argp );
int getpid_getppid_pipe_test( void * the_argp );
int getpriority_setpriority_test( void * the_argp );
int getrusage_test( void * the_argp );
int groups_test( void * the_argp );
int ioctl_test( void * the_argp );
int kqueue_tests( void * the_argp );
int limit_tests( void * the_argp );
int link_stat_unlink_test( void * the_argp );
int locking_test( void * the_argp );
int memory_tests( void * the_argp );
int message_queue_tests( void * the_argp );
int mkdir_rmdir_umask_test( void * the_argp );
int mkfifo_test( void * the_argp );
int mknod_sync_test( void * the_argp );
int open_close_test( void * the_argp );
int process_group_test( void * the_argp );
int quotactl_test( void * the_argp );
int read_write_test( void * the_argp );
int rename_test( void * the_argp );
int searchfs_test( void * the_argp );
int sema_tests( void * the_argp );
int sema2_tests( void * the_argp );
int shm_tests( void * the_argp );
int signals_test( void * the_argp );
int socket_tests( void * the_argp );
int socket2_tests( void * the_argp );
int syscall_test( void * the_argp );
int time_tests( void * the_argp );
int uid_tests( void * the_argp );
int xattr_tests( void * the_argp );
int data_exec_tests( void * the_argp );
int machvm_tests( void * the_argp );
int getdirentries_test( void * the_argp );
int statfs_32bit_inode_tests( void * the_argp );
int commpage_data_tests( void * the_argp );
int atomic_fifo_queue_test( void * the_argp );
int sched_tests( void * the_argp );
int content_protection_test( void * the_argp );
int pipes_test( void * the_argp );
int kaslr_test( void * the_argp );
int getattrlistbulk_test( void * the_argp );
int openat_close_test( void * the_argp );
int linkat_fstatat_unlinkat_test( void * the_argp );
int faccessat_fchmodat_fchmod_test( void * the_argp );
int fchownat_fchown_symlinkat_test( void * the_argp );
int mkdirat_unlinkat_umask_test( void * the_argp );
int renameat_test( void * the_argp );
int set_exception_ports_test( void * the_argp );

struct test_entry 
{
	int				test_run_it;		/* 0 means do not run this test, else run it */
	test_rtn_t		test_routine;		/* routine to call */
	void *			test_input;			/* optional input to test_routine */ 
	char *			test_infop;			/* information about what is tested */ 
};
typedef struct test_entry * test_entryp;

/* Special replacement printf with date/time stamp */
int my_printf(const char * __restrict fmt, ...);
#define printf my_printf

/* 
   If running xnu_quick_test under testbots, disable special 
   printf defined in the previous step. This is done in order
   to generate log messages in a format which testbots understands
*/ 

#if RUN_UNDER_TESTBOTS
#undef printf
#endif

#endif /* !_TESTS_H_ */
