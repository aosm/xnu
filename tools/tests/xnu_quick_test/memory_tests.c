/*
 *  memory_tests.c.c
 *  xnu_quick_test
 *
 *  Created by Jerry Cottingham on 4/12/05.
 *  Copyright 2005 Apple Computer Inc. All rights reserved.
 *
 */

#include "tests.h"
#include <mach/mach.h>

extern char  g_target_path[ PATH_MAX ];

/*
 * static to localize to this compilation unit; volatile to avoid register
 * optimization which would prevent modification by a signal handler.
 */
static volatile int	my_err;

void
bus_handler(int sig, siginfo_t *si, void *mcontext)
{
	/* Reset global error value when we see a SIGBUS */
	if (sig == SIGBUS) {
		_exit(0);
	}
}

/*  **************************************************************************************************************
 *	Test madvise, mincore, minherit, mlock, mlock, mmap, mprotect, msync, munmap system calls.
 *	todo - see if Francois has better versions of these tests...
 *  **************************************************************************************************************
 */
int memory_tests( void * the_argp )
{
	int			my_page_size, my_status;
	int			my_fd = -1;
	char *		my_pathp = NULL;
	char *		my_bufp = NULL;
	char *		my_addr = NULL;
	char *		my_test_page_p = NULL;
	ssize_t		my_result;
	pid_t		my_pid, my_wait_pid;
	kern_return_t   my_kr;		
	struct sigaction	my_sa;

        my_kr = vm_allocate((vm_map_t) mach_task_self(), (vm_address_t*)&my_pathp, PATH_MAX, VM_FLAGS_ANYWHERE);
        if(my_kr != KERN_SUCCESS){
                printf( "vm_allocate failed with error %d - \"%s\" \n", errno, strerror( errno) );
                goto test_failed_exit;
        }
	
	*my_pathp = 0x00;
	strcat( my_pathp, &g_target_path[0] );
	strcat( my_pathp, "/" );

	/* create a test file */
	my_err = create_random_name( my_pathp, 1 );
	if ( my_err != 0 ) {
		goto test_failed_exit;
	}

	my_page_size = getpagesize( );
	my_kr = vm_allocate((vm_map_t) mach_task_self(), (vm_address_t*)&my_test_page_p, my_page_size, VM_FLAGS_ANYWHERE);
        if(my_kr != KERN_SUCCESS){
                printf( "vm_allocate failed with error %d - \"%s\" \n", errno, strerror( errno) );
                goto test_failed_exit;
        }

	*my_test_page_p = 0x00;
	strcat( my_test_page_p, "parent data" );
 
	/* test minherit - share a page with child, add to the string in child then 
	 * check for modification after child terminates.
	 */ 
	my_err = minherit( my_test_page_p, my_page_size, VM_INHERIT_SHARE );
	if ( my_err == -1 ) {
		printf( "minherit failed with error %d - \"%s\" \n", errno, strerror( errno) );
		goto test_failed_exit;
	}

	/*
	 * spin off a child process that we will use for testing.   
	 */
	my_pid = fork( );
	if ( my_pid == -1 ) {
		printf( "fork failed with errno %d - %s \n", errno, strerror( errno ) );
		goto test_failed_exit;
	}
	if ( my_pid == 0 ) {
		/* 
		 * child process...
		 */		
		strcat( my_test_page_p, " child data" );

		/* create a test file in page size chunks */
       		my_kr = vm_allocate((vm_map_t) mach_task_self(), (vm_address_t*)&my_bufp, (my_page_size * 10), VM_FLAGS_ANYWHERE);
	        if(my_kr != KERN_SUCCESS){
        	        printf( "vm_allocate failed with error %d - \"%s\" \n", errno, strerror( errno) );
			my_err = -1;
                	goto exit_child;
        	}

		/* test madvise on anonymous memory */
		my_err = madvise(my_bufp, (my_page_size * 10), MADV_WILLNEED);
		if ( my_err == -1 ) {
			printf("madvise WILLNEED on anon memory failed with error %d - \"%s\" \n", errno, strerror( errno ) );
			my_err = -1;
			goto exit_child;
		}

		memset( my_bufp, 'j', (my_page_size * 10) );
		my_fd = open( my_pathp, O_RDWR, 0 );
		if ( my_fd == -1 ) {
			printf( "open call failed with error %d - \"%s\" \n", errno, strerror( errno) );
			my_err = -1;
			goto exit_child;
		}
		
		/* test madvise on anonymous memory */
		my_err = madvise(my_bufp, (my_page_size * 10), MADV_DONTNEED);
		if ( my_err == -1 ) {
			printf("madvise DONTNEED on anon memory failed with error %d - \"%s\" \n", errno, strerror( errno ) );
			my_err = -1;
			goto exit_child;
		}

		my_result = write( my_fd, my_bufp, (my_page_size * 10) );
		if ( my_result == -1 ) {
			printf( "write call failed with error %d - \"%s\" \n", errno, strerror( errno) );
			my_err = -1;
			goto exit_child;
		}
		
		/* map the file into memory */
		my_addr = (char *) mmap( NULL, (my_page_size * 2), (PROT_READ | PROT_WRITE), (MAP_FILE | MAP_SHARED), my_fd, 0 );
		if ( my_addr == (char *) -1 ) {
			printf( "mmap call failed with error %d - \"%s\" \n", errno, strerror( errno) );
			my_err = -1;
			goto exit_child;
		}
		
		/* make sure we got the right data mapped */
		if ( *my_addr != 'j' || *(my_addr + my_page_size) != 'j' ) {
			printf( "did not map in correct data \n" );
			my_err = -1;
			goto exit_child;
		}

		/* test madvise */
		my_err = madvise( my_addr, (my_page_size * 2), MADV_WILLNEED );
		if ( my_err == -1 ) {
			printf( "madvise WILLNEED call failed with error %d - \"%s\" \n", errno, strerror( errno) );
			my_err = -1;
			goto exit_child;
		}

		my_err = madvise( my_addr, (my_page_size * 2), MADV_DONTNEED );
		if ( my_err == -1 ) {
			printf( "madvise DONTNEED call failed with error %d - \"%s\" \n", errno, strerror( errno) );
			my_err = -1;
			goto exit_child;
		}

		/* test mincore, mlock, mlock */
		my_err = mlock( my_addr, my_page_size );
		if ( my_err == -1 ) {
			printf( "mlock call failed with error %d - \"%s\" \n", errno, strerror( errno) );
			my_err = -1;
			goto exit_child;
		}

		/* mybufp is about to be reused, so test madvise on anonymous memory */
		my_err = madvise(my_bufp, (my_page_size * 10), MADV_FREE);
		if ( my_err == -1 ) {
			printf("madvise FREE on anon memory failed with error %d - \"%s\" \n", errno, strerror( errno ) );
			my_err = -1;
			goto exit_child;
		}

		my_err = mincore( my_addr, 1, my_bufp );	
		if ( my_err == -1 ) {
			printf( "mincore call failed with error %d - \"%s\" \n", errno, strerror( errno) );
			my_err = -1;
			goto exit_child;
		}
		/* page my_addr is in should be resident after mlock */
		if ( (*my_bufp & MINCORE_INCORE) == 0 ) {
			printf( "mincore call failed to find resident page \n" );
			my_err = -1;
			goto exit_child;
		}
		
		my_err = munlock( my_addr, my_page_size );
		if ( my_err == -1 ) {
			printf( "munlock call failed with error %d - \"%s\" \n", errno, strerror( errno) );
			my_err = -1;
			goto exit_child;
		}
		 
		/* modify first page then use msync to push data out */
		memset( my_addr, 'x', my_page_size );
		my_err = msync( my_addr, my_page_size, (MS_SYNC | MS_INVALIDATE) );	
		if ( my_err == -1 ) {
			printf( "msync call failed with error %d - \"%s\" \n", errno, strerror( errno) );
			my_err = -1;
			goto exit_child;
		}
					
		/* test madvise */
		my_err = madvise( my_addr, (my_page_size * 2), MADV_DONTNEED );
		if ( my_err == -1 ) {
			printf( "madvise DONTNEED call failed with error %d - \"%s\" \n", errno, strerror( errno) );
			my_err = -1;
			goto exit_child;
		}

		/* test madvise */
		my_err = madvise( my_addr, (my_page_size * 2), MADV_FREE );
		if ( my_err == -1 ) {
			printf( "madvise FREE call failed with error %d - \"%s\" \n", errno, strerror( errno) );
			my_err = -1;
			goto exit_child;
		}

		/* verify that the file was updated */
		lseek( my_fd, 0, SEEK_SET );	
		bzero( (void *)my_bufp, my_page_size );
		my_result = read( my_fd, my_bufp, my_page_size );
		if ( my_result == -1 ) {
			printf( "read call failed with error %d - \"%s\" \n", errno, strerror( errno) );
			my_err = -1;
			goto exit_child;
		}
		if ( *my_bufp != 'x' ) {
			printf( "msync did not flush correct data \n" );
			my_err = -1;
			goto exit_child;
		}
		 
		/* unmap our test page */
		my_err = munmap( my_addr, (my_page_size * 2) );
		if ( my_err == -1 ) {
			printf( "munmap call failed with error %d - \"%s\" \n", errno, strerror( errno) );
			my_err = -1;
			goto exit_child;
		}
		my_addr = NULL;

		/* map the file into memory again for mprotect test  */
		my_addr = (char *) mmap( NULL, (my_page_size * 2), (PROT_READ | PROT_WRITE), (MAP_FILE | MAP_SHARED), my_fd, 0 );
		if ( my_addr == (char *) -1 ) {
			printf( "mmap call failed with error %d - \"%s\" \n", errno, strerror( errno) );
			my_err = -1;
			goto exit_child;
		}
		*my_addr = 'a';
	 


		/* test mprotect - change protection to only PROT_READ */
		my_err = mprotect( my_addr, my_page_size, PROT_READ );
		if ( my_err == -1 ) {
			printf( "mprotect call failed with error %d - \"%s\" \n", errno, strerror( errno) );
			my_err = -1;
			goto exit_child;
		}

		my_sa.sa_sigaction = bus_handler;
		my_sa.sa_flags = SA_SIGINFO | SA_RESETHAND;
		if ((my_err = sigaction(SIGBUS, &my_sa, NULL)) != 0) {
			printf("sigaction call failed with error %d - \"%s\" \n", errno, strerror( errno) );
			my_err = -1;
			goto exit_child;
		}

		my_err = -1;	/* default to error out if we do NOT trigger a SIGBUS */

		*my_addr = 'z'; /* should cause SIGBUS signal (we look for this at child termination within the parent) */

		/* NOTREACHED */

		printf("Expected SIGBUS signal, got nothing!\n");
		my_err = -1;
exit_child:
		exit( my_err );
	}

	/* parent process -
	 * we should get no error if the child has completed all tests successfully
	 */
	my_wait_pid = wait4( my_pid, &my_status, 0, NULL );
	if ( my_wait_pid == -1 ) {
		printf( "wait4 failed with errno %d - %s \n", errno, strerror( errno ) );
		goto test_failed_exit;
	}

	/* wait4 should return our child's pid when it exits */
	if ( my_wait_pid != my_pid ) {
		printf( "wait4 did not return child pid - returned %d should be %d \n", my_wait_pid, my_pid );
		goto test_failed_exit;
	}

	/* If we did not exit cleanly, report it
	 */
	if ( !WIFEXITED( my_status ) || (WEXITSTATUS( my_status ) != 0)) {
		printf( "wait4 returned child died of status - 0x%08X \n", my_status );
		goto test_failed_exit;
	}

	/* make sure shared page got modified in child */
	if ( strcmp( my_test_page_p, "parent data child data" ) != 0 ) {
		printf( "minherit did not work correctly - shared page looks wrong \n" );
		goto test_failed_exit;
	}
	my_err = 0;
	goto test_passed_exit;

test_failed_exit:
	my_err = -1;
	
test_passed_exit:
	if ( my_pathp != NULL ) {
		remove( my_pathp );	
		vm_deallocate(mach_task_self(), (vm_address_t)my_pathp, PATH_MAX);		
	 }
	 if ( my_test_page_p != NULL ) {
		vm_deallocate(mach_task_self(), (vm_address_t)my_test_page_p, my_page_size);
	 }
	return( my_err );
}

