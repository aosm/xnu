#include <string.h>
#include <errno.h>
#include <pwd.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <poll.h>
#include <sys/types.h>
#include <sys/event.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/xattr.h>

#define DIR1 	"dir1"
#define DOTDOT 	".."
#define DIR2 	"dir2"
#define FILE1	"file1"
#define FILE2 	"file2"

#define KEY	"somekey"
#define VAL	"someval"

#define NOSLEEP		0
#define SLEEP		1
#define NO_EVENT	0
#define YES_EVENT	1


#define OUTPUT_LEVEL 	2
#define RESULT_LEVEL	3

#define TEST_STRING	"Some text!!! Yes indeed, some of that very structure which has passed on man's knowledge for generations."
#define HELLO_WORLD	"Hello, World!"
#define SLEEP_TIME	2
#define WAIT_TIME	(4l)
#define LENGTHEN_SIZE	500
#define FIFO_SPACE	8192	/* FIFOS have 8K of buffer space */

/*
 * Types of actions for setup, cleanup, and execution of tests
 */
typedef enum {CREAT, MKDIR, READ, WRITE, WRITEFD, FILLFD, UNLINK, LSKEE, RMDIR, MKFIFO, LENGTHEN, TRUNC,
	SYMLINK, CHMOD, CHOWN, EXCHANGEDATA, RENAME, LSEEK, OPEN, MMAP, NOTHING,
	SETXATTR, UTIMES, STAT, HARDLINK, REVOKE} action_id_t;

/* 
 * Directs an action as mentioned above
 */
typedef struct _action {
	int 		act_dosleep;
	action_id_t 	act_id;
	void 		*act_args[5];
	int		act_fd;
} action_t;

/*
 * A test case.  Specifies setup, an event to look for, an action to take to
 * cause (or not cause) that event, and cleanup.
 */
typedef struct _test {
	char *t_testname;
	
	/* Test kevent() or poll() */
	int 	t_is_poll_test;	
	
	/* Actions for setting up test */
	int 	 t_n_prep_actions;
	action_t t_prep_actions[5];
	
	/* Actions for cleaning up test */
	int 	 t_n_cleanup_actions;
	action_t t_cleanup_actions[5];
	
	/* Action for thred to take while we wait */
	action_t t_helpthreadact;
	
	/* File to look for event on */
	char 	 *t_watchfile; 	/* set event ident IN TEST (can't know fd beforehand)*/
	int	 t_file_is_fifo;/* FIFOs are handled in a special manner */
	
	/* Different parameters for poll() vs kevent() */
	union { 
		struct kevent	tu_kev;
		short		tu_pollevents;
	} t_union;
	
	/* Do we expect results? */
	int	 t_want_event;
	
	/* Not always used--how much data should we find (EVFILT_{READ,WRITE}) */
	int	 t_nbytes;
	
	/* Hacks for FILT_READ and pipes */
	int 	 t_read_to_end_first; 	/* Consume all data in file before waiting for event */
	int 	 t_write_some_data; 	/* Write some data to file before waiting for event (FIFO hack) */
	int	 t_extra_sleep_hack;	/* Sleep before waiting, to let a fifo fill up with data */
} test_t;

/*
 * Extra logging infrastructure so we can filter some out
 */
void LOG(int level, FILE *f, const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	if (level >= OUTPUT_LEVEL) {
		/* Indent for ease of reading */
		if (level < RESULT_LEVEL) {
			fprintf(f, "\t");
		}
		vfprintf(f, fmt, ap);
	} 
	
	va_end(ap);
}

/*
 * Initialize an action struct.  Whether to sleep, what action to take,
 * and arguments for that action.
 */
void 
init_action(action_t *act, int sleep, action_id_t call, int nargs, ...) 
{
	int i;
	va_list ap;
	va_start(ap, nargs);
	act->act_dosleep = sleep;
	act->act_id = call;
	
	for (i = 0; i < nargs; i++)
	{
		act->act_args[i] = va_arg(ap, void*);
	}
	
	va_end(ap);
	
}

/*
 * Opening a fifo is complicated: need to open both sides at once 
 */
void*
open_fifo_readside(void *arg) 
{
	return (void*)open((char*)arg, O_RDONLY);
}

/*
 * Open a fifo, setting read and write descriptors.  Return 0 for success, -1 for failure.
 * Only set FD args upon success; they will be unmodified on failure.
 */
int 
open_fifo(const char *path, int *readfd, int *writefd) 
{
	pthread_t thread;
	int waitres;
	int res;
	int tmpreadfd, tmpwritefd;
	
	res = pthread_create(&thread, 0, open_fifo_readside, (void*)path);
	if (res == 0) {
		tmpwritefd = open(path, O_WRONLY);
		waitres = pthread_join(thread, (void**) &tmpreadfd);
		
		fcntl(tmpwritefd, F_SETFL, O_WRONLY | O_NONBLOCK);
		
		if ((waitres == 0) && (tmpwritefd >= 0) && (tmpreadfd >= 0)) {
			*readfd = tmpreadfd;
			*writefd = tmpwritefd;
		} else {
			res = -1;	
		}
	}
	
	return res;
}

/*
 * Just concatenate a directory and a filename, sticking a "/" betwixt them
 */
void 
makepath(char *buf, const char *dir, const char *file) 
{
	strcpy(buf, dir);
	strcat(buf, "/");
	strcat(buf, file);
}


/* Execute a prep, cleanup, or test action; specific tricky notes below.
 *
 * CREAT: 	comes to life and given length 1
 * READ: 	try to read one char
 * WRITE:	try to write TEST_STRING to file
 * LENGTHEN:	make longer by LENGTHEN_SIZE
 * MMAP:	mmap first 20 bytes of file, write HELLO_WORLD in
 * SETXATTR:	set the KEY attribute to value VAL
 * WRITEFD:	instead of opening fresh, take an FD in the action struct (FIFOs)
 * FILLFD:	write a file until you can no longer.  for filling FIFOS.
 *
 * * Several of these have hard-coded sizes.
 */
void* 
execute_action(void *actionptr) 
{
	action_t *act = (action_t*)actionptr;
	void **args = act->act_args;
	char c;
	int res = -1, tmpfd, tmpfd2;
	static int lastfd;
	void *addr;
	struct timeval tv;
	struct stat sstat;
	
	LOG(1, stderr, "Beginning action of type %d\n", act->act_id);
	
	/* Let other thread get into kevent() sleep */
	if(SLEEP == act->act_dosleep) {
		sleep(SLEEP_TIME); 
	}
	switch(act->act_id) {
		case NOTHING:
			res = 0;
			break;
		case CREAT:
			tmpfd = creat((char*)args[0], 0755);
			ftruncate(tmpfd, 1); /* So that mmap() doesn't fool us */
			if (tmpfd >= 0) {
				close(tmpfd);
				res = 0;
			}
			break;
		case MKDIR:
			res = mkdir((char*)args[0], 0755);
			break;
		case READ:
			tmpfd = open((char*)args[0], O_RDONLY);
			if (tmpfd >= 0) {
				res = read(tmpfd, &c, 1);
				res = (res == 1 ? 0 : -1);
			}
			close(tmpfd);
			break;
		case WRITE:
			tmpfd = open((char*)args[0], O_RDWR);
			if (tmpfd >= 0) {
				res = write(tmpfd, TEST_STRING, strlen(TEST_STRING));
				if (res == strlen(TEST_STRING)) {
					res = 0;
				} else {
					res = -1;
				}
				
				close(tmpfd);
			}
			break;
		case WRITEFD:
			res = write((int)act->act_fd, TEST_STRING, strlen(TEST_STRING));
			if (res == strlen(TEST_STRING)) {
				res = 0;
			} else {
				res = -1;
			}
			break;
		case FILLFD:
			while (write((int)act->act_fd, "a", 1) > 0);
			res = 0;
			break;
		case UNLINK:
			res = unlink((char*)args[0]);
			break;
		case LSEEK:
			res = lseek((int)act->act_fd, (int)args[0], SEEK_SET);
			res = (res == (int)args[0] ? 0 : -1);
			break;
		case RMDIR:
			res = rmdir((char*)args[0]);
			break;
		case MKFIFO:
			res = mkfifo((char*)args[0], 0755);
			break;
		case LENGTHEN:
			res = truncate((char*)args[0], LENGTHEN_SIZE);
			break;
		case TRUNC:
			res = truncate((char*)args[0], 0);
			break;
		case SYMLINK:
			res = symlink((char*)args[0], (char*)args[1]);
			break;
		case CHMOD:
			res = chmod((char*)args[0], (int)args[1]);
			break;
		case CHOWN:
			/* path, uid, gid */
			res = chown((char*)args[0], (int) args[1], (int) args[2]);
			break;
		case EXCHANGEDATA:
			res = exchangedata((char*)args[0], (char*)args[1], 0);
			break;
		case RENAME:
			res = rename((char*)args[0], (char*)args[1]);
			break;
		case OPEN:
			tmpfd = open((char*)args[0], O_RDONLY | O_CREAT);
			res = close(tmpfd);
			break;
		case MMAP:
			/* It had best already exist with nonzero size */
			tmpfd = open((char*)args[0], O_RDWR);
			addr = mmap(0, 20, PROT_WRITE | PROT_READ, MAP_FILE | MAP_SHARED, tmpfd, 0);
			if (addr != ((void*)-1)) {
				res = 0;
				if ((int)args[1]) {
					strcpy((char*)addr, HELLO_WORLD);
					msync(addr, 20, MS_SYNC);
				}
			}
			close(tmpfd);
			munmap(addr, 20);
			break;
		case SETXATTR:
			res = setxattr((char*)args[0], KEY, (void*)VAL, strlen(VAL),
						   0, 0);
			break;
		case UTIMES:
			tv.tv_sec = time(NULL);
			tv.tv_usec = 0;
			res = utimes((char*)args[0], &tv); 
			break;
		case STAT:
			res = lstat((char*)args[0], &sstat);
			break;
		case HARDLINK:
			res = link((char*)args[0], (char*)args[1]);
			break;
		case REVOKE:
			tmpfd = open((char*)args[0], O_RDONLY);
			res = revoke((char*)args[0]);
			close(tmpfd);
			break;
		default:
			res = -1;
			break;
	}
	
	return (void*)res;
	
}

/*
 * Read until the end of a file, for EVFILT_READ purposes (considers file position)
 */
void 
read_to_end(int fd) 
{
	char buf[50];
	while (read(fd, buf, sizeof(buf)) > 0);
}

/*
 * Helper for setup and cleanup; just execute every action in an array
 * of actions.  "failout" parameter indicates whether to stop if one fails.
 */
int
execute_action_list(action_t *actions, int nactions, int failout) 
{
	int i, res;
	for (i = 0, res = 0; (0 == res || (!failout)) && (i < nactions); i++) {
		LOG(1, stderr, "Starting prep action %d\n", i);
		res = (int) execute_action(&(actions[i]));
		if(res != 0) {
			LOG(2, stderr, "Action list failed on step %d.\n", i);
		} else {
			LOG(1, stderr, "Action list work succeeded on step %d.\n", i);
		}
	}
	
	return res;
}

/*
 * Execute a full test, return success value.
 */
int
execute_test(test_t *test)
{
	int i, kqfd, filefd = -1, res2, res, cnt, status, writefd = -1;
	int retval = -1;
	pthread_t thr;
	struct kevent evlist;
	struct timespec ts = {WAIT_TIME, 0l};
	
	memset(&evlist, 0, sizeof(evlist));
	
	LOG(1, stderr, "Test %s starting.\n", test->t_testname);
	LOG(1, stderr, test->t_want_event ? "Expecting an event.\n" : "Not expecting events.\n");
	
	res = execute_action_list(test->t_prep_actions, test->t_n_prep_actions, 1);
	
	/* If prep succeeded */
	if (0 == res) {
		/* Create kqueue for kqueue tests*/
		if (!test->t_is_poll_test) {
			kqfd = kqueue(); 
		}
		
		if ((test->t_is_poll_test) || kqfd >= 0) {
			LOG(1, stderr, "Opened kqueue.\n");
			
			/* Open the file we're to monitor.  Fifos get special handling */
			if (test->t_file_is_fifo) {
				filefd = -1;
				open_fifo(test->t_watchfile, &filefd, &writefd);
			} else {
				filefd = open(test->t_watchfile, O_RDONLY | O_SYMLINK);
			}
			
			if (filefd >= 0) {
				LOG(1, stderr, "Opened file to monitor.\n");
				
				/* 
				 * Fill in the fd to monitor once you know it 
				 * If it's a fifo test, then the helper is definitely going to want the write end.
				 */
				test->t_helpthreadact.act_fd = (writefd >= 0 ? writefd : filefd);
				
				if (test->t_read_to_end_first) {
					read_to_end(filefd);
				} else if (test->t_write_some_data) {
					action_t dowr;
					init_action(&dowr, NOSLEEP, WRITEFD, 0);
					dowr.act_fd = writefd;
					execute_action(&dowr);
				}
				
				/* Helper modifies the file that we're listening on (sleeps first, in general) */
				res = pthread_create(&thr, NULL, execute_action, (void*) &test->t_helpthreadact);
				if (0 == res) {
					LOG(1, stderr, "Created helper thread.\n");
					
					/* This is ugly business to hack on filling up a FIFO */
					if (test->t_extra_sleep_hack) {
						sleep(5);
					}
					
					if (test->t_is_poll_test) {
						struct pollfd pl;
						pl.fd = filefd;
						pl.events = test->t_union.tu_pollevents;
						cnt = poll(&pl, 1, WAIT_TIME);
						LOG(1, stderr, "Finished poll() call.\n");
						
						if ((cnt < 0)) {
							LOG(2, stderr, "error is in errno, %s\n", strerror(errno));
							res = cnt;
						}
					} else {
						test->t_union.tu_kev.ident = filefd; 
						cnt = kevent(kqfd, &test->t_union.tu_kev, 1, &evlist, 1,  &ts);
						LOG(1, stderr, "Finished kevent() call.\n");
						
						if ((cnt < 0) || (evlist.flags & EV_ERROR))  {
							LOG(2, stderr, "kevent() call failed.\n");
							if (cnt < 0) {
								LOG(2, stderr, "error is in errno, %s\n", strerror(errno));
							} else {
								LOG(2, stderr, "error is in data, %s\n", strerror(evlist.data));
							}
							res = cnt;
						}
					}
					
					/* Success only if you've succeeded to this point AND joined AND other thread is happy*/
					status = 0;
					res2 = pthread_join(thr, (void**)&status);
					if (res2 < 0) {
						LOG(2, stderr, "Couldn't join helper thread.\n"); 
					} else if (status) {
						LOG(2, stderr, "Helper action had result %d\n", (int)status);
					}
					res = ((res == 0) && (res2 == 0) && (status == 0)) ? 0 : -1;
				} else {
					LOG(2, stderr, "Couldn't start thread.\n");
				}
				
				close(filefd);
				if (test->t_file_is_fifo) {
					close(writefd);
				}
			} else {
				LOG(2, stderr, "Couldn't open test file %s to monitor.\n", test->t_watchfile);
				res = -1;
			}
			close(kqfd);
		} else {
			LOG(2, stderr, "Couldn't open kqueue.\n");
			res = -1;
		}
	}
	
	/* Cleanup work */
	execute_action_list(test->t_cleanup_actions, test->t_n_cleanup_actions, 0);
	
	/* Success if nothing failed and we either received or did not receive event,
	 * as expected 
	 */
	if (0 == res) {
		LOG(1, stderr, cnt > 0 ? "Got an event.\n" : "Did not get an event.\n");
		if (((cnt > 0) && (test->t_want_event)) || ((cnt == 0) && (!test->t_want_event))) {
			if ((!test->t_is_poll_test) && (test->t_union.tu_kev.filter == EVFILT_READ || test->t_union.tu_kev.filter == EVFILT_WRITE)
				&& (test->t_nbytes) && (test->t_nbytes != evlist.data)) {
				LOG(2, stderr, "Read wrong number of bytes available.  Wanted %d, got %d\n", test->t_nbytes, evlist.data);
				retval = -1;
			} else {
				retval = 0;
			}
			
		} else {
			LOG(2, stderr, "Got unexpected event or lack thereof.\n");
			retval = -1;
		}
	} else {
		LOG(2, stderr, "Failed to execute test.\n");
		retval = -1;
	}
	
	LOG(3, stdout, "Test %s done with result %d.\n", test->t_testname, retval);
}

void
init_test_common(test_t *tst, char *testname, char *watchfile, int nprep, int nclean, int event, int want, int ispoll)
{
	memset(tst, 0, sizeof(test_t));
	tst->t_testname = testname;
	tst->t_watchfile = watchfile;
	tst->t_n_prep_actions = nprep;
	tst->t_n_cleanup_actions = nclean;
	tst->t_want_event = (want > 0);
	
	if (ispoll) {
		tst->t_is_poll_test = 1;
		tst->t_union.tu_pollevents = (short)event;
	} else {
		/* Can do this because filter is negative, notes are positive */
		if (event == EVFILT_READ || event == EVFILT_WRITE) {
			EV_SET(&tst->t_union.tu_kev, 0, event, EV_ADD | EV_ENABLE, 0, 0, NULL);
			tst->t_nbytes = want;
		} else {
			EV_SET(&tst->t_union.tu_kev, 0, EVFILT_VNODE, EV_ADD | EV_ENABLE, event, 0, NULL);
		}
	}
}

/*
 * Initialize a test case, not including its actions.  Meaning: a name for it, what filename to watch,
 * counts of prep and cleanup actions, what event to watch for, and whether you want an event/how many bytes read.
 *
 * "want" does double duty as whether you want an event and how many bytes you might want to read
 * "event" is either an event flag (e.g. NOTE_WRITE) or EVFILT_READ
 */	
void 
init_test(test_t *tst, char *testname, char *watchfile, int nprep, int nclean, int event, int want) 
{
	init_test_common(tst, testname, watchfile, nprep, nclean, event, want, 0);
}

/*
 * Same as above, but for a poll() test
 */
void
init_poll_test(test_t *tst, char *testname, char *watchfile, int nprep, int nclean, int event, int want) 
{
	init_test_common(tst, testname, watchfile, nprep, nclean, event, want, 1);
}

void 
run_note_delete_tests() 
{
	test_t test;
	
	init_test(&test, "1.1.2: unlink a file", FILE1, 1, 0, NOTE_DELETE, YES_EVENT);
	init_action(&(test.t_prep_actions[0]), NOSLEEP, CREAT, 2, (void*)FILE1, (void*)NULL);
	init_action(&test.t_helpthreadact, SLEEP, UNLINK, 2, (void*)FILE1, NULL);
	execute_test(&test);
	
	init_test(&test, "1.1.3: rmdir a dir", DIR1, 1, 0, NOTE_DELETE, YES_EVENT);
	init_action(&(test.t_prep_actions[0]), NOSLEEP, MKDIR, 2, (void*)DIR1, (void*)NULL);
	init_action(&test.t_helpthreadact, SLEEP, RMDIR, 2, (void*)DIR1, NULL);
	execute_test(&test);
	
	init_test(&test, "1.1.4: rename one file over another", FILE2, 2, 1, NOTE_DELETE, YES_EVENT);
	init_action(&(test.t_prep_actions[0]), NOSLEEP, CREAT, 2, (void*)FILE1, (void*)NULL);
	init_action(&(test.t_prep_actions[1]), NOSLEEP, CREAT, 2, (void*)FILE2, (void*)NULL);
	init_action(&test.t_helpthreadact, SLEEP, RENAME, 2, (void*)FILE1, (void*)FILE2);
	init_action(&test.t_cleanup_actions[0], NOSLEEP, UNLINK, 2, (void*)FILE2, NULL);
	execute_test(&test);
	
	init_test(&test, "1.1.5: rename one dir over another", DIR2, 2, 1, NOTE_DELETE, YES_EVENT);
	init_action(&(test.t_prep_actions[0]), NOSLEEP, MKDIR, 2, (void*)DIR1, (void*)NULL);
	init_action(&(test.t_prep_actions[1]), NOSLEEP, MKDIR, 2, (void*)DIR2, (void*)NULL);
	init_action(&test.t_helpthreadact, SLEEP, RENAME, 2, (void*)DIR1, (void*)DIR2);
	init_action(&test.t_cleanup_actions[0], NOSLEEP, RMDIR, 2, (void*)DIR2, NULL);
	execute_test(&test);
	
	/* Do FIFO stuff here */
	init_test(&test, "1.1.6: make a fifo, unlink it", FILE1, 1, 0, NOTE_DELETE, YES_EVENT);
	test.t_file_is_fifo = 1;
	init_action(&(test.t_prep_actions[0]), NOSLEEP, MKFIFO, 2, (void*)FILE1, (void*)NULL);
	init_action(&test.t_helpthreadact, SLEEP, UNLINK, 1, (void*)FILE1);
	execute_test(&test);
	
	init_test(&test, "1.1.7: rename a file over a fifo", FILE1, 2, 1, NOTE_DELETE, YES_EVENT);
	test.t_file_is_fifo = 1;
	init_action(&(test.t_prep_actions[0]), NOSLEEP, MKFIFO, 2, (void*)FILE1, (void*)NULL);
	init_action(&(test.t_prep_actions[1]), NOSLEEP, CREAT, 2, (void*)FILE2, (void*)NULL);
	init_action(&test.t_helpthreadact, SLEEP, RENAME, 2, (void*)FILE2, (void*)FILE1);
	init_action(&test.t_cleanup_actions[0], NOSLEEP, UNLINK, 2, (void*)FILE1, NULL);
	execute_test(&test);
	
	init_test(&test, "1.1.8: unlink a symlink to a file", FILE2, 2, 1, NOTE_DELETE, YES_EVENT);
	init_action(&(test.t_prep_actions[0]), NOSLEEP, CREAT, 2, (void*)FILE1, (void*)NULL);
	init_action(&(test.t_prep_actions[1]), NOSLEEP, SYMLINK, 2, (void*)FILE1, (void*)FILE2);
	init_action(&test.t_helpthreadact, SLEEP, UNLINK, 2, (void*)FILE2, NULL);
	init_action(&test.t_cleanup_actions[0], NOSLEEP, UNLINK, 2, (void*)FILE1, NULL);
	execute_test(&test);
	
	/* ================= */
	
	init_test(&test, "1.2.1: Straight-up rename file", FILE1, 1, 1, NOTE_DELETE, NO_EVENT);
	init_action(&(test.t_prep_actions[0]), NOSLEEP, CREAT, 2, (void*)FILE1, (void*)NULL);
	init_action(&test.t_helpthreadact, SLEEP, RENAME, 2, (void*)FILE1, (void*)FILE2); 
	init_action(&test.t_cleanup_actions[0], NOSLEEP, UNLINK, 2, (void*)FILE2, (void*)NULL);
	execute_test(&test);
	
	init_test(&test, "1.2.2: Straight-up rename dir", DIR1, 1, 1, NOTE_DELETE, NO_EVENT);
	init_action(&(test.t_prep_actions[0]), NOSLEEP, MKDIR, 2, (void*)DIR1, (void*)NULL);
	init_action(&test.t_helpthreadact, SLEEP, RENAME, 2, (void*)DIR1, (void*)DIR2); 
	init_action(&test.t_cleanup_actions[0], NOSLEEP, RMDIR, 2, (void*)DIR2, (void*)NULL);
	execute_test(&test);
	
	init_test(&test, "1.2.3: Null action on file", FILE1, 1, 1, NOTE_DELETE, NO_EVENT);
	init_action(&(test.t_prep_actions[0]), NOSLEEP, CREAT, 2, (void*)FILE1, (void*)NULL);
	init_action(&test.t_helpthreadact, SLEEP, NOTHING, 2, NULL, NULL); /* The null action */
	init_action(&test.t_cleanup_actions[0], NOSLEEP, UNLINK, 2, (void*)FILE1, (void*)NULL);
	execute_test(&test);
	
	init_test(&test, "1.2.4: Rename one file over another: watch the file that lives", FILE1, 2, 1, NOTE_DELETE, NO_EVENT);
	init_action(&(test.t_prep_actions[0]), NOSLEEP, CREAT, 2, (void*)FILE1, (void*)NULL);
	init_action(&(test.t_prep_actions[1]), NOSLEEP, CREAT, 2, (void*)FILE2, (void*)NULL);
	init_action(&test.t_helpthreadact, SLEEP, RENAME, 2, (void*)FILE1, (void*)FILE2);
	init_action(&test.t_cleanup_actions[0], NOSLEEP, UNLINK, 2, (void*)FILE2, NULL);
	execute_test(&test);
	
	init_test(&test, "1.2.5: Rename one dir over another, watch the dir that lives", DIR1, 2, 1, NOTE_DELETE, NO_EVENT);
	init_action(&(test.t_prep_actions[0]), NOSLEEP, MKDIR, 2, (void*)DIR1, (void*)NULL);
	init_action(&(test.t_prep_actions[1]), NOSLEEP, MKDIR, 2, (void*)DIR2, (void*)NULL);
	init_action(&test.t_helpthreadact, SLEEP, RENAME, 2, (void*)DIR1, (void*)DIR2);
	init_action(&test.t_cleanup_actions[0], NOSLEEP, RMDIR, 2, (void*)DIR2, NULL);
}

void 
run_note_write_tests()
{
	char pathbuf[50];
	char otherpathbuf[50];
	
	test_t test;
	
	init_test(&test, "2.1.1: Straight-up write to a file", FILE1, 1, 1, NOTE_WRITE, YES_EVENT);
	init_action(&(test.t_prep_actions[0]), NOSLEEP, CREAT, 2, (void*)FILE1, (void*)NULL);
	init_action(&test.t_helpthreadact, SLEEP, WRITE, 2, (void*)FILE1, NULL);
	init_action(&test.t_cleanup_actions[0], NOSLEEP, UNLINK, 2, (void*)FILE1, (void*)NULL);
	execute_test(&test);
	
	
	makepath(pathbuf, DIR1, FILE1);
	init_test(&test, "2.1.2: creat() file inside a dir", DIR1, 1, 2, NOTE_WRITE, YES_EVENT);
	init_action(&(test.t_prep_actions[0]), NOSLEEP, MKDIR, 2, (void*)DIR1, (void*)NULL);
	init_action(&test.t_helpthreadact, SLEEP, CREAT, 2, (void*)pathbuf, NULL);
	init_action(&test.t_cleanup_actions[0], NOSLEEP, UNLINK, 2, (void*)pathbuf, (void*)NULL);
	init_action(&test.t_cleanup_actions[1], NOSLEEP, RMDIR, 2, (void*)DIR1, (void*)NULL);
	execute_test(&test);
	
	makepath(pathbuf, DIR1, FILE1);
	init_test(&test, "2.1.3: open() file inside a dir", DIR1, 1, 2, NOTE_WRITE, YES_EVENT);
	init_action(&(test.t_prep_actions[0]), NOSLEEP, MKDIR, 2, (void*)DIR1, (void*)NULL);
	init_action(&test.t_helpthreadact, SLEEP, OPEN, 2, (void*)pathbuf, NULL);
	init_action(&test.t_cleanup_actions[0], NOSLEEP, UNLINK, 2, (void*)pathbuf, (void*)NULL);
	init_action(&test.t_cleanup_actions[1], NOSLEEP, RMDIR, 2, (void*)DIR1, (void*)NULL);
	execute_test(&test);
	
	makepath(pathbuf, DIR1, FILE1);
	init_test(&test, "2.1.3: unlink a file from a dir", DIR1, 2, 1, NOTE_WRITE, YES_EVENT);
	init_action(&(test.t_prep_actions[0]), NOSLEEP, MKDIR, 2, (void*)DIR1, (void*)NULL);
	init_action(&(test.t_prep_actions[1]), NOSLEEP, CREAT, 2, (void*)pathbuf, (void*)NULL);
	init_action(&test.t_helpthreadact, SLEEP, UNLINK, 2, (void*)pathbuf, NULL);
	init_action(&test.t_cleanup_actions[0], NOSLEEP, RMDIR, 2, (void*)DIR1, (void*)NULL);
	execute_test(&test);
	
	makepath(pathbuf, DIR1, FILE1);
	makepath(otherpathbuf, DIR1, FILE2);
	init_test(&test, "2.1.5: rename a file in a dir", DIR1, 2, 2, NOTE_WRITE, YES_EVENT);
	init_action(&(test.t_prep_actions[0]), NOSLEEP, MKDIR, 2, (void*)DIR1, (void*)NULL);
	init_action(&(test.t_prep_actions[1]), NOSLEEP, CREAT, 2, (void*)pathbuf, (void*)NULL);
	init_action(&test.t_helpthreadact, SLEEP, RENAME, 2, (void*)pathbuf, (void*)otherpathbuf);
	init_action(&test.t_cleanup_actions[0], NOSLEEP, UNLINK, 2, (void*)otherpathbuf, (void*)NULL);
	init_action(&test.t_cleanup_actions[1], NOSLEEP, RMDIR, 2, (void*)DIR1, (void*)NULL);
	execute_test(&test);
	
	makepath(pathbuf, DIR1, FILE1);
	init_test(&test, "2.1.6: rename a file to outside of a dir", DIR1, 2, 2, NOTE_WRITE, YES_EVENT);
	init_action(&(test.t_prep_actions[0]), NOSLEEP, MKDIR, 2, (void*)DIR1, (void*)NULL);
	init_action(&(test.t_prep_actions[1]), NOSLEEP, CREAT, 2, (void*)pathbuf, (void*)NULL);
	init_action(&test.t_helpthreadact, SLEEP, RENAME, 2, (void*)pathbuf, (void*)FILE1);
	init_action(&test.t_cleanup_actions[0], NOSLEEP, UNLINK, 2, (void*)FILE1, (void*)NULL);
	init_action(&test.t_cleanup_actions[1], NOSLEEP, RMDIR, 2, (void*)DIR1, (void*)NULL);
	execute_test(&test);
	
	makepath(pathbuf, DIR1, FILE1);
	init_test(&test, "2.1.7: rename a file into a dir", DIR1, 2, 2, NOTE_WRITE, YES_EVENT);
	init_action(&(test.t_prep_actions[0]), NOSLEEP, MKDIR, 2, (void*)DIR1, (void*)NULL);
	init_action(&(test.t_prep_actions[1]), NOSLEEP, CREAT, 2, (void*)FILE1, (void*)NULL);
	init_action(&test.t_helpthreadact, SLEEP, RENAME, 2, (void*)FILE1, (void*)pathbuf);
	init_action(&test.t_cleanup_actions[0], NOSLEEP, UNLINK, 2, (void*)pathbuf, (void*)NULL);
	init_action(&test.t_cleanup_actions[1], NOSLEEP, RMDIR, 2, (void*)DIR1, (void*)NULL);
	execute_test(&test);
	
	makepath(pathbuf, DIR1, FILE1);
	init_test(&test, "2.1.9: unlink a fifo from a dir", DIR1, 2, 1, NOTE_WRITE, YES_EVENT);
	init_action(&(test.t_prep_actions[0]), NOSLEEP, MKDIR, 2, (void*)DIR1, (void*)NULL);
	init_action(&(test.t_prep_actions[1]), NOSLEEP, MKFIFO, 2, (void*)pathbuf, (void*)NULL);
	init_action(&test.t_helpthreadact, SLEEP, UNLINK, 2, (void*)pathbuf, NULL);
	init_action(&test.t_cleanup_actions[0], NOSLEEP, RMDIR, 2, (void*)DIR1, (void*)NULL);
	execute_test(&test);
	
	makepath(pathbuf, DIR1, FILE1);
	init_test(&test, "2.1.10: make symlink in a dir", DIR1, 1, 2, NOTE_WRITE, YES_EVENT);
	init_action(&(test.t_prep_actions[0]), NOSLEEP, MKDIR, 2, (void*)DIR1, (void*)NULL);
	init_action(&test.t_helpthreadact, SLEEP, SYMLINK, 2, (void*)DOTDOT, (void*)pathbuf);
	init_action(&test.t_cleanup_actions[0], NOSLEEP, UNLINK, 2, (void*)pathbuf, (void*)NULL);
	init_action(&test.t_cleanup_actions[1], NOSLEEP, RMDIR, 2, (void*)DIR1, (void*)NULL);
	execute_test(&test);
	
	init_test(&test, "2.1.12: write to a FIFO", FILE1, 1, 1, NOTE_WRITE, YES_EVENT);
	init_action(&(test.t_prep_actions[0]), NOSLEEP, MKFIFO, 2, (void*)FILE1, (void*)NULL);
	test.t_file_is_fifo = 1;
	init_action(&test.t_helpthreadact, SLEEP, WRITEFD, 0);
	init_action(&test.t_cleanup_actions[0], NOSLEEP, UNLINK, 2, (void*)FILE1, (void*)NULL);
	execute_test(&test);
	
	
	makepath(pathbuf, DIR1, FILE1);
	init_test(&test, "2.1.13: delete a symlink in a dir", DIR1, 2, 1, NOTE_WRITE, YES_EVENT);
	init_action(&(test.t_prep_actions[0]), NOSLEEP, MKDIR, 2, (void*)DIR1, (void*)NULL);
	init_action(&(test.t_prep_actions[1]), NOSLEEP, SYMLINK, 2, (void*)DOTDOT, (void*)pathbuf);
	init_action(&test.t_helpthreadact, SLEEP, UNLINK, 2, (void*)pathbuf, (void*)FILE1);
	init_action(&test.t_cleanup_actions[0], NOSLEEP, RMDIR, 2, (void*)DIR1, (void*)NULL);
	execute_test(&test);
	
	/* This actually should not generate an event, though it's in this section */
	makepath(pathbuf, DIR1, FILE1);
	makepath(otherpathbuf, DIR1, FILE2);
	init_test(&test, "2.1.14: exchangedata two files in a dir", DIR1, 3, 3, NOTE_WRITE, NO_EVENT);
	init_action(&(test.t_prep_actions[0]), NOSLEEP, MKDIR, 2, (void*)DIR1, (void*)NULL);
	init_action(&(test.t_prep_actions[1]), NOSLEEP, CREAT, 2, (void*)pathbuf, (void*)NULL);
	init_action(&(test.t_prep_actions[2]), NOSLEEP, CREAT, 2, (void*)otherpathbuf, (void*)NULL);
	init_action(&test.t_helpthreadact, SLEEP, EXCHANGEDATA, 2, (void*)pathbuf, (void*)otherpathbuf);
	init_action(&test.t_cleanup_actions[0], NOSLEEP, UNLINK, 2, (void*)pathbuf, (void*)NULL);
	init_action(&test.t_cleanup_actions[1], NOSLEEP, UNLINK, 2, (void*)otherpathbuf, (void*)NULL);
	init_action(&test.t_cleanup_actions[2], NOSLEEP, RMDIR, 2, (void*)DIR1, (void*)NULL);
	execute_test(&test);
	
	LOG(1, stderr, "MMAP test should fail on HFS.\n");
	init_test(&test, "2.1.15: Change a file with mmap()", FILE1, 1, 1, NOTE_WRITE, YES_EVENT);
	init_action(&(test.t_prep_actions[0]), NOSLEEP, CREAT, 2, (void*)FILE1, (void*)NULL);
	init_action(&test.t_helpthreadact, SLEEP, MMAP, 2, (void*)FILE1, (void*)1); /* 1 -> "modify it"*/
	init_action(&test.t_cleanup_actions[0], NOSLEEP, UNLINK, 2, (void*)FILE1, (void*)NULL);
	execute_test(&test);
	
	/*================= no-event tests ==================*/
	init_test(&test, "2.2.1: just open and close existing file", FILE1, 1, 1, NOTE_WRITE, NO_EVENT);
	init_action(&(test.t_prep_actions[0]), NOSLEEP, CREAT, 2, (void*)FILE1, (void*)NULL);
	init_action(&test.t_helpthreadact, SLEEP, OPEN, 2, (void*)FILE1, NULL);
	init_action(&test.t_cleanup_actions[0], NOSLEEP, UNLINK, 2, (void*)FILE1, (void*)NULL);
	execute_test(&test);
	
	init_test(&test, "2.2.2: read from existing file", FILE1, 1, 1, NOTE_WRITE, NO_EVENT);
	init_action(&(test.t_prep_actions[0]), NOSLEEP, CREAT, 2, (void*)FILE1, (void*)NULL);
	init_action(&test.t_helpthreadact, SLEEP, READ, 2, (void*)FILE1, NULL);
	init_action(&test.t_cleanup_actions[0], NOSLEEP, UNLINK, 2, (void*)FILE1, (void*)NULL);
	execute_test(&test);
	
	init_test(&test, "2.2.3: rename existing file", FILE1, 1, 1, NOTE_WRITE, NO_EVENT);
	init_action(&(test.t_prep_actions[0]), NOSLEEP, CREAT, 2, (void*)FILE1, (void*)NULL);
	init_action(&test.t_helpthreadact, SLEEP, RENAME, 2, (void*)FILE1, (void*)FILE2);
	init_action(&test.t_cleanup_actions[0], NOSLEEP, UNLINK, 2, (void*)FILE2, (void*)NULL);
	execute_test(&test);
	
	init_test(&test, "2.2.4: just open and close dir", DIR1, 1, 1, NOTE_WRITE, NO_EVENT);
	init_action(&(test.t_prep_actions[0]), NOSLEEP, MKDIR, 2, (void*)DIR1, (void*)NULL);
	init_action(&test.t_helpthreadact, SLEEP, OPEN, 2, (void*)DIR1, (void*)NULL);
	init_action(&test.t_cleanup_actions[0], NOSLEEP, RMDIR, 2, (void*)DIR1, (void*)NULL);
	execute_test(&test);
	
	/* There are no tests 2.2.5 or 2.2.6 */
	
	init_test(&test, "2.2.7: rename a dir", DIR1, 1, 1, NOTE_WRITE, NO_EVENT);
	init_action(&(test.t_prep_actions[0]), NOSLEEP, MKDIR, 2, (void*)DIR1, (void*)NULL);
	init_action(&test.t_helpthreadact, SLEEP, RENAME, 2, (void*)DIR1, (void*)DIR2);
	init_action(&test.t_cleanup_actions[0], NOSLEEP, RMDIR, 2, (void*)DIR2, (void*)NULL);
	execute_test(&test);
	
	init_test(&test, "2.2.8: rename a fifo", FILE1, 1, 1, NOTE_WRITE, NO_EVENT);
	test.t_file_is_fifo = 1;
	init_action(&(test.t_prep_actions[0]), NOSLEEP, MKFIFO, 2, (void*)FILE1, (void*)NULL);
	init_action(&test.t_helpthreadact, SLEEP, RENAME, 2, (void*)FILE1, (void*)FILE2);
	init_action(&test.t_cleanup_actions[0], NOSLEEP, UNLINK, 2, (void*)FILE2, (void*)NULL);
	execute_test(&test);
	
	init_test(&test, "2.2.9: unlink a fifo", FILE1, 1, 0, NOTE_WRITE, NO_EVENT);
	test.t_file_is_fifo = 1;
	init_action(&(test.t_prep_actions[0]), NOSLEEP, MKFIFO, 2, (void*)FILE1, (void*)NULL);
	init_action(&test.t_helpthreadact, SLEEP, UNLINK,1, (void*)FILE1);
	execute_test(&test);
	
	init_test(&test, "2.2.10: chmod a file", FILE1, 1, 1, NOTE_WRITE, NO_EVENT);
	init_action(&(test.t_prep_actions[0]), NOSLEEP, CREAT, 2, (void*)FILE1, (void*)NULL);
	init_action(&test.t_helpthreadact, SLEEP, CHMOD, 2, (void*)FILE1, (void*)0700);
	init_action(&test.t_cleanup_actions[0], NOSLEEP, UNLINK, 2, (void*)FILE1, (void*)NULL);
	execute_test(&test);
	
	struct passwd *pwd = getpwnam("local");
	int uid = pwd->pw_uid;
	int gid = pwd->pw_gid;
	
	init_test(&test, "2.2.11: chown a file", FILE1, 2, 1, NOTE_WRITE, NO_EVENT);
	init_action(&(test.t_prep_actions[0]), NOSLEEP, CREAT, 2, (void*)FILE1, (void*)NULL);
	init_action(&test.t_prep_actions[1], NOSLEEP, CHOWN, 3, (void*)FILE1, (void*)uid, (void*)gid);
	init_action(&test.t_helpthreadact, SLEEP, CHOWN, 3, (void*)FILE1, (void*)getuid(), (void*)getgid());
	init_action(&test.t_cleanup_actions[0], NOSLEEP, UNLINK, 2, (void*)FILE1, (void*)NULL);
	execute_test(&test);
	
	
	init_test(&test, "2.2.12: chmod a dir", DIR1, 1, 1, NOTE_WRITE, NO_EVENT);
	init_action(&(test.t_prep_actions[0]), NOSLEEP, MKDIR, 2, (void*)DIR1, (void*)NULL);
	init_action(&test.t_helpthreadact, SLEEP, CHMOD, 2, (void*)DIR1, (void*)0700);
	init_action(&test.t_cleanup_actions[0], NOSLEEP, RMDIR, 2, (void*)DIR1, (void*)NULL);
	execute_test(&test);
	
	init_test(&test, "2.2.13: chown a dir", DIR1, 2, 1, NOTE_WRITE, NO_EVENT);
	init_action(&(test.t_prep_actions[0]), NOSLEEP, MKDIR, 2, (void*)DIR1, (void*)NULL);
	init_action(&test.t_prep_actions[1], NOSLEEP, CHOWN, 3, (void*)DIR1, (void*)uid, (void*)gid);
	init_action(&test.t_helpthreadact, SLEEP, CHOWN, 3, (void*)DIR1, (void*)getuid(), (void*)getgid());
	init_action(&test.t_cleanup_actions[0], NOSLEEP, RMDIR, 2, (void*)DIR1, (void*)NULL);
	execute_test(&test);
	
	
	
	LOG(1, stderr, "MMAP will never give a notification on HFS.\n");
	init_test(&test, "2.1.14: mmap() a file but do not change it", FILE1, 1, 1, NOTE_WRITE, NO_EVENT);
	init_action(&(test.t_prep_actions[0]), NOSLEEP, CREAT, 2, (void*)FILE1, (void*)NULL);
	init_action(&test.t_helpthreadact, SLEEP, MMAP, 2, (void*)FILE1, (void*)0); 
	init_action(&test.t_cleanup_actions[0], NOSLEEP, UNLINK, 2, (void*)FILE1, (void*)NULL);
	execute_test(&test);
}

void
run_note_extend_tests()
{
	test_t test;
	char pathbuf[50];
	
	LOG(1, stderr, "THESE TESTS WILL FAIL ON HFS!\n");
	
	init_test(&test, "3.1.1: write beyond the end of a file", FILE1, 1, 1, NOTE_EXTEND, YES_EVENT);
	init_action(&(test.t_prep_actions[0]), NOSLEEP, CREAT, 2, (void*)FILE1, (void*)NULL);
	init_action(&test.t_helpthreadact, SLEEP, WRITE, 2, (void*)FILE1, (void*)NULL); 
	init_action(&test.t_cleanup_actions[0], NOSLEEP, UNLINK, 2, (void*)FILE1, (void*)NULL);
	execute_test(&test);
	
	/*
	 * We won't concern ourselves with lengthening directories: commenting these out  
	 *
	 
	 makepath(pathbuf, DIR1, FILE1);
	 init_test(&test, "3.1.2: add a file to a directory with creat()", DIR1, 1, 2, NOTE_EXTEND, YES_EVENT);
	 init_action(&(test.t_prep_actions[0]), NOSLEEP, MKDIR, 2, (void*)DIR1, (void*)NULL);
	 init_action(&test.t_helpthreadact, SLEEP, CREAT, 2, (void*)pathbuf, (void*)NULL); 
	 init_action(&test.t_cleanup_actions[0], NOSLEEP, UNLINK, 2, (void*)pathbuf, (void*)NULL);
	 init_action(&test.t_cleanup_actions[1], NOSLEEP, RMDIR, 2, (void*)DIR1, (void*)NULL);
	 execute_test(&test);
	 
	 makepath(pathbuf, DIR1, FILE1);
	 init_test(&test, "3.1.3: add a file to a directory with open()", DIR1, 1, 2, NOTE_EXTEND, YES_EVENT);
	 init_action(&(test.t_prep_actions[0]), NOSLEEP, MKDIR, 2, (void*)DIR1, (void*)NULL);
	 init_action(&test.t_helpthreadact, SLEEP, CREAT, 2, (void*)pathbuf, (void*)NULL); 
	 init_action(&test.t_cleanup_actions[0], NOSLEEP, UNLINK, 2, (void*)pathbuf, (void*)NULL);
	 init_action(&test.t_cleanup_actions[1], NOSLEEP, RMDIR, 2, (void*)DIR1, (void*)NULL);
	 execute_test(&test);
	 
	 makepath(pathbuf, DIR1, FILE1);
	 init_test(&test, "3.1.4: add a file to a directory with rename()", DIR1, 2, 2, NOTE_EXTEND, YES_EVENT);
	 init_action(&(test.t_prep_actions[0]), NOSLEEP, CREAT, 2, (void*)FILE1, (void*)NULL);
	 init_action(&(test.t_prep_actions[1]), NOSLEEP, MKDIR, 2, (void*)DIR1, (void*)NULL);
	 init_action(&test.t_helpthreadact, SLEEP, RENAME, 2, (void*)FILE1, (void*)pathbuf); 
	 init_action(&test.t_cleanup_actions[0], NOSLEEP, UNLINK, 2, (void*)pathbuf, (void*)NULL);
	 init_action(&test.t_cleanup_actions[1], NOSLEEP, RMDIR, 2, (void*)DIR1, (void*)NULL);
	 execute_test(&test);
	 */
	
	/* 3.1.5: a placeholder for a potential kernel test */
	/*
	 makepath(pathbuf, DIR1, DIR2);
	 init_test(&test, "3.1.6: add a file to a directory with mkdir()", DIR1, 1, 2, NOTE_EXTEND, YES_EVENT);
	 init_action(&(test.t_prep_actions[0]), NOSLEEP, MKDIR, 2, (void*)DIR1, (void*)NULL);
	 init_action(&test.t_helpthreadact, SLEEP, MKDIR, 2, (void*)pathbuf, (void*)NULL); 
	 init_action(&test.t_cleanup_actions[0], NOSLEEP, RMDIR, 2, (void*)pathbuf, (void*)NULL);
	 init_action(&test.t_cleanup_actions[1], NOSLEEP, RMDIR, 2, (void*)DIR1, (void*)NULL);
	 execute_test(&test);
	 */
	init_test(&test, "3.1.7: lengthen a file with truncate()", FILE1, 1, 1, NOTE_EXTEND, YES_EVENT);
	init_action(&(test.t_prep_actions[0]), NOSLEEP, CREAT, 2, (void*)FILE1, (void*)NULL);
	init_action(&test.t_helpthreadact, SLEEP, LENGTHEN, 2, FILE1, (void*)NULL); 
	init_action(&test.t_cleanup_actions[0], NOSLEEP, UNLINK, 2, (void*)FILE1, (void*)NULL);
	execute_test(&test);
	
	
	/** ========== NO EVENT SECTION ============== **/
	init_test(&test, "3.2.1: setxattr() a file", FILE1, 1, 1, NOTE_EXTEND, NO_EVENT);
	init_action(&(test.t_prep_actions[0]), NOSLEEP, CREAT, 2, (void*)FILE1, (void*)NULL);
	init_action(&test.t_helpthreadact, SLEEP, SETXATTR, 2, FILE1, (void*)NULL); 
	init_action(&test.t_cleanup_actions[0], NOSLEEP, UNLINK, 2, (void*)FILE1, (void*)NULL);
	execute_test(&test);
	
	init_test(&test, "3.2.2: chmod a file", FILE1, 1, 1, NOTE_EXTEND, NO_EVENT);
	init_action(&(test.t_prep_actions[0]), NOSLEEP, CREAT, 2, (void*)FILE1, (void*)NULL);
	init_action(&test.t_helpthreadact, SLEEP, CHMOD, 2, (void*)FILE1, (void*)0700);
	init_action(&test.t_cleanup_actions[0], NOSLEEP, UNLINK, 2, (void*)FILE1, (void*)NULL);
	execute_test(&test);
	
	struct passwd *pwd = getpwnam("local");
	if (!pwd) {
		LOG(2, stderr, "Couldn't getpwnam for local.\n");
		exit(1);
	} 	
	int uid = pwd->pw_uid;
	int gid = pwd->pw_gid;
	
	init_test(&test, "3.2.3: chown a file", FILE1, 2, 1, NOTE_EXTEND, NO_EVENT);
	init_action(&(test.t_prep_actions[0]), NOSLEEP, CREAT, 2, (void*)FILE1, (void*)NULL);
	init_action(&test.t_prep_actions[1], NOSLEEP, CHOWN, 3, (void*)FILE1, (void*)uid, (void*)gid);
	init_action(&test.t_helpthreadact, SLEEP, CHOWN, 3, (void*)FILE1, (void*)getuid(), (void*)getgid());
	init_action(&test.t_cleanup_actions[0], NOSLEEP, UNLINK, 2, (void*)FILE1, (void*)NULL);
	execute_test(&test);
	
	
	init_test(&test, "3.2.4: chmod a dir", DIR1, 1, 1, NOTE_EXTEND, NO_EVENT);
	init_action(&(test.t_prep_actions[0]), NOSLEEP, MKDIR, 2, (void*)DIR1, (void*)NULL);
	init_action(&test.t_helpthreadact, SLEEP, CHMOD, 2, (void*)DIR1, (void*)0700);
	init_action(&test.t_cleanup_actions[0], NOSLEEP, RMDIR, 2, (void*)DIR1, (void*)NULL);
	execute_test(&test);
	
	init_test(&test, "3.2.5: chown a dir", DIR1, 2, 1, NOTE_EXTEND, NO_EVENT);
	init_action(&(test.t_prep_actions[0]), NOSLEEP, MKDIR, 2, (void*)DIR1, (void*)NULL);
	init_action(&test.t_prep_actions[1], NOSLEEP, CHOWN, 3, (void*)DIR1, (void*)uid, (void*)gid);
	init_action(&test.t_helpthreadact, SLEEP, CHOWN, 3, (void*)DIR1, (void*)getuid(), (void*)getgid());
	init_action(&test.t_cleanup_actions[0], NOSLEEP, RMDIR, 2, (void*)DIR1, (void*)NULL);
	execute_test(&test);
	
	init_test(&test, "3.2.6: TRUNC a file with truncate()", FILE1, 1, 1, NOTE_EXTEND, NO_EVENT);
	init_action(&(test.t_prep_actions[0]), NOSLEEP, CREAT, 2, (void*)FILE1, (void*)NULL);
	init_action(&test.t_helpthreadact, SLEEP, TRUNC, 2, FILE1, (void*)NULL); 
	init_action(&test.t_cleanup_actions[0], NOSLEEP, UNLINK, 2, (void*)FILE1, (void*)NULL);
	execute_test(&test);
}

void
run_note_attrib_tests()
{
	test_t test;
	char pathbuf[50];
	
	init_test(&test, "4.1.1: chmod a file", FILE1, 1, 1, NOTE_ATTRIB, YES_EVENT);
	init_action(&(test.t_prep_actions[0]), NOSLEEP, CREAT, 2, (void*)FILE1, (void*)NULL);
	init_action(&test.t_helpthreadact, SLEEP, CHMOD, 2, FILE1, (void*)0700); 
	init_action(&test.t_cleanup_actions[0], NOSLEEP, UNLINK, 2, (void*)FILE1, (void*)NULL);
	execute_test(&test);
	
	struct passwd *pwd = getpwnam("local");
	int uid = pwd->pw_uid;
	int gid = pwd->pw_gid;
	
	init_test(&test, "4.1.2: chown a file", FILE1, 2, 1, NOTE_ATTRIB, YES_EVENT);
	init_action(&(test.t_prep_actions[0]), NOSLEEP, CREAT, 2, (void*)FILE1, (void*)NULL);
	init_action(&(test.t_prep_actions[1]), NOSLEEP, CHOWN, 3, (void*)FILE1, (void*)uid, (void*)gid);
	init_action(&test.t_helpthreadact, SLEEP, CHOWN, 3, FILE1, (void*)getuid(), (void*)gid); 
	init_action(&test.t_cleanup_actions[0], NOSLEEP, UNLINK, 2, (void*)FILE1, (void*)NULL);
	execute_test(&test);
	
	init_test(&test, "4.1.3: chmod a dir", DIR1, 1, 1, NOTE_ATTRIB, YES_EVENT);
	init_action(&(test.t_prep_actions[0]), NOSLEEP, MKDIR, 2, (void*)DIR1, (void*)NULL);
	init_action(&(test.t_helpthreadact), SLEEP, CHMOD, 2, (void*)DIR1, (void*)0700);
	init_action(&test.t_cleanup_actions[0], NOSLEEP, RMDIR, 2, (void*)DIR1, (void*)NULL);
	execute_test(&test);
	
	init_test(&test, "4.1.4: chown a dir", DIR1, 2, 1, NOTE_ATTRIB, YES_EVENT);
	init_action(&(test.t_prep_actions[0]), NOSLEEP, MKDIR, 2, (void*)DIR1, (void*)NULL);
	init_action(&(test.t_prep_actions[1]), NOSLEEP, CHOWN, 3, (void*)DIR1, (void*) uid, (void*)gid);
	init_action(&test.t_helpthreadact, SLEEP, CHOWN, 3, DIR1, (void*)getuid(), (void*)getgid()); 
	init_action(&test.t_cleanup_actions[0], NOSLEEP, RMDIR, 2, (void*)DIR1, (void*)NULL);
	execute_test(&test);
	
	init_test(&test, "4.1.5: setxattr on a file", FILE1, 1, 1, NOTE_ATTRIB, YES_EVENT);
	init_action(&(test.t_prep_actions[0]), NOSLEEP, CREAT, 2, (void*)FILE1, (void*)NULL);
	init_action(&test.t_helpthreadact, SLEEP, SETXATTR, 2, (void*)FILE1, (void*)NULL); 
	init_action(&test.t_cleanup_actions[0], NOSLEEP, UNLINK, 2, (void*)FILE1, (void*)NULL);
	execute_test(&test);
	
	init_test(&test, "4.1.6: setxattr on a dir", DIR1, 1, 1, NOTE_ATTRIB, YES_EVENT);
	init_action(&(test.t_prep_actions[0]), NOSLEEP, MKDIR, 2, (void*)DIR1, (void*)NULL);
	init_action(&test.t_helpthreadact, SLEEP, SETXATTR, 2, (void*)DIR1, (void*)NULL); 
	init_action(&test.t_cleanup_actions[0], NOSLEEP, RMDIR, 2, (void*)DIR1, (void*)NULL);
	execute_test(&test);
	
	
	init_test(&test, "4.1.7: exchangedata", FILE1, 2, 2, NOTE_ATTRIB, YES_EVENT);
	init_action(&(test.t_prep_actions[0]), NOSLEEP, CREAT, 2, (void*)FILE1, (void*)NULL);
	init_action(&(test.t_prep_actions[1]), NOSLEEP, CREAT, 2, (void*)FILE2, (void*)NULL);
	init_action(&test.t_helpthreadact, SLEEP, EXCHANGEDATA, 2, (void*)FILE1, (void*)FILE2); 
	init_action(&test.t_cleanup_actions[0], NOSLEEP, UNLINK, 2, (void*)FILE1, (void*)NULL);
	init_action(&test.t_cleanup_actions[1], NOSLEEP, UNLINK, 2, (void*)FILE2, (void*)NULL);
	execute_test(&test);
	
	
	init_test(&test, "4.1.8: utimes on a file", FILE1, 1, 1, NOTE_ATTRIB, YES_EVENT);
	init_action(&(test.t_prep_actions[0]), NOSLEEP, CREAT, 2, (void*)FILE1, (void*)NULL);
	init_action(&test.t_helpthreadact, SLEEP, UTIMES, 2, (void*)FILE1, (void*)NULL); 
	init_action(&test.t_cleanup_actions[0], NOSLEEP, UNLINK, 2, (void*)FILE1, (void*)NULL);
	execute_test(&test);
	
	init_test(&test, "4.1.9: utimes on a dir", DIR1, 1, 1, NOTE_ATTRIB, YES_EVENT);
	init_action(&(test.t_prep_actions[0]), NOSLEEP, MKDIR, 2, (void*)DIR1, (void*)NULL);
	init_action(&test.t_helpthreadact, SLEEP, UTIMES, 2, (void*)DIR1, (void*)NULL); 
	init_action(&test.t_cleanup_actions[0], NOSLEEP, RMDIR, 2, (void*)DIR1, (void*)NULL);
	execute_test(&test);
	
	
	/* ====== NO EVENT TESTS ========== */
	
	init_test(&test, "4.2.1: rename a file", FILE1, 1, 1, NOTE_ATTRIB, NO_EVENT);
	init_action(&(test.t_prep_actions[0]), NOSLEEP, CREAT, 2, (void*)FILE1, (void*)NULL);
	init_action(&test.t_helpthreadact, SLEEP, RENAME, 2, (void*)FILE1, (void*)FILE2);
	init_action(&test.t_cleanup_actions[0], NOSLEEP, UNLINK, 2, (void*)FILE2, NULL);
	execute_test(&test);
	
	init_test(&test, "4.2.2: open (do not change) a file", FILE1, 1, 1, NOTE_ATTRIB, NO_EVENT);
	init_action(&(test.t_prep_actions[0]), NOSLEEP, CREAT, 2, (void*)FILE1, (void*)NULL);
	init_action(&test.t_helpthreadact, SLEEP, OPEN, 2, (void*)FILE1, NULL);
	init_action(&test.t_cleanup_actions[0], NOSLEEP, UNLINK, 2, (void*)FILE1, NULL);
	execute_test(&test);
	
	init_test(&test, "4.2.3: stat a file", FILE1, 1, 1, NOTE_ATTRIB, NO_EVENT);
	init_action(&(test.t_prep_actions[0]), NOSLEEP, CREAT, 2, (void*)FILE1, (void*)NULL);
	init_action(&test.t_helpthreadact, SLEEP, STAT, 2, (void*)FILE1, NULL);
	init_action(&test.t_cleanup_actions[0], NOSLEEP, UNLINK, 2, (void*)FILE1, NULL);
	execute_test(&test);
	
	init_test(&test, "4.2.4: unlink a file", FILE1, 1, 0, NOTE_ATTRIB, NO_EVENT);
	init_action(&(test.t_prep_actions[0]), NOSLEEP, CREAT, 2, (void*)FILE1, (void*)NULL);
	init_action(&test.t_helpthreadact, SLEEP, UNLINK, 2, (void*)FILE1, NULL);
	execute_test(&test);
	
	init_test(&test, "4.2.5: write to a file", FILE1, 1, 1, NOTE_ATTRIB, NO_EVENT);
	init_action(&(test.t_prep_actions[0]), NOSLEEP, CREAT, 2, (void*)FILE1, (void*)NULL);
	init_action(&test.t_helpthreadact, SLEEP, WRITE, 2, (void*)FILE1, (void*)NULL); 
	init_action(&test.t_cleanup_actions[0], NOSLEEP, UNLINK, 2, (void*)FILE1, (void*)NULL);
	execute_test(&test);
	
	LOG(1, stderr, "EXPECT SPURIOUS NOTE_ATTRIB EVENTS FROM DIRECTORY OPERATIONS on HFS.\n");
	init_test(&test, "4.2.6: add a file to a directory with creat()", DIR1, 1, 2, NOTE_ATTRIB, NO_EVENT);
	makepath(pathbuf, DIR1, FILE1);
	init_action(&(test.t_prep_actions[0]), NOSLEEP, MKDIR, 2, (void*)DIR1, (void*)NULL);
	init_action(&test.t_helpthreadact, SLEEP, CREAT, 2, (void*)pathbuf, (void*)NULL); 
	init_action(&test.t_cleanup_actions[0], NOSLEEP, UNLINK, 2, (void*)pathbuf, (void*)NULL);
	init_action(&test.t_cleanup_actions[1], NOSLEEP, RMDIR, 2, (void*)DIR1, (void*)NULL);
	execute_test(&test);
	
	init_test(&test, "4.2.7: mkdir in a dir", DIR1, 1, 2, NOTE_ATTRIB, NO_EVENT);
	makepath(pathbuf, DIR1, DIR2);
	init_action(&(test.t_prep_actions[0]), NOSLEEP, MKDIR, 2, (void*)DIR1, (void*)NULL);
	init_action(&test.t_helpthreadact, SLEEP, MKDIR, 2, (void*)pathbuf, (void*)NULL); 
	init_action(&test.t_cleanup_actions[0], NOSLEEP, RMDIR, 2, (void*)pathbuf, (void*)NULL);
	init_action(&test.t_cleanup_actions[1], NOSLEEP, RMDIR, 2, (void*)DIR1, (void*)NULL);
	execute_test(&test);
	
	init_test(&test, "4.2.8: add a symlink to a directory", DIR1, 1, 2, NOTE_ATTRIB, NO_EVENT);
	makepath(pathbuf, DIR1, FILE1);
	init_action(&(test.t_prep_actions[0]), NOSLEEP, MKDIR, 2, (void*)DIR1, (void*)NULL);
	init_action(&test.t_helpthreadact, SLEEP, SYMLINK, 2, (void*)DOTDOT, (void*)pathbuf); 
	init_action(&test.t_cleanup_actions[0], NOSLEEP, UNLINK, 2, (void*)pathbuf, (void*)NULL);
	init_action(&test.t_cleanup_actions[1], NOSLEEP, RMDIR, 2, (void*)DIR1, (void*)NULL);
	execute_test(&test);
	
	init_test(&test, "4.2.9: rename into a dir()", DIR1, 2, 2, NOTE_ATTRIB, NO_EVENT);
	makepath(pathbuf, DIR1, FILE1);
	init_action(&(test.t_prep_actions[0]), NOSLEEP, MKDIR, 2, (void*)DIR1, (void*)NULL);
	init_action(&(test.t_prep_actions[1]), NOSLEEP, CREAT, 2, (void*)FILE1, (void*)NULL);
	init_action(&test.t_helpthreadact, SLEEP, RENAME, 2, (void*)FILE1, (void*)pathbuf); 
	init_action(&test.t_cleanup_actions[0], NOSLEEP, UNLINK, 2, (void*)pathbuf, (void*)NULL);
	init_action(&test.t_cleanup_actions[1], NOSLEEP, RMDIR, 2, (void*)DIR1, (void*)NULL);
	execute_test(&test);
	
	init_test(&test, "4.2.10: unlink() file from dir", DIR1, 2, 1, NOTE_ATTRIB, NO_EVENT);
	makepath(pathbuf, DIR1, FILE1);
	init_action(&(test.t_prep_actions[0]), NOSLEEP, MKDIR, 2, (void*)DIR1, (void*)NULL);
	init_action(&(test.t_prep_actions[1]), NOSLEEP, CREAT, 2, (void*)pathbuf, (void*)NULL);
	init_action(&test.t_helpthreadact, SLEEP, UNLINK, 2, (void*)pathbuf, (void*)NULL); 
	init_action(&test.t_cleanup_actions[0], NOSLEEP, RMDIR, 2, (void*)DIR1, (void*)NULL);
	execute_test(&test);
	
	init_test(&test, "4.2.11: mkfifo in a directory", DIR1, 1, 2, NOTE_ATTRIB, NO_EVENT);
	makepath(pathbuf, DIR1, FILE1);
	init_action(&(test.t_prep_actions[0]), NOSLEEP, MKDIR, 2, (void*)DIR1, (void*)NULL);
	init_action(&test.t_helpthreadact, SLEEP, MKFIFO, 1, (void*)pathbuf); 
	init_action(&test.t_cleanup_actions[0], NOSLEEP, UNLINK, 2, (void*)pathbuf, (void*)NULL);
	init_action(&test.t_cleanup_actions[1], NOSLEEP, RMDIR, 2, (void*)DIR1, (void*)NULL);
	execute_test(&test);
	
	
}


void 
run_note_link_tests()
{
	test_t test;
	char pathbuf[50];
	char otherpathbuf[50];
	
	LOG(1, stderr, "HFS DOES NOT HANDLE UNLINK CORRECTLY...\n");
	init_test(&test, "5.1.1: unlink() a file", FILE1, 1, 0, NOTE_LINK, YES_EVENT);
	init_action(&(test.t_prep_actions[0]), NOSLEEP, CREAT, 2, (void*)FILE1, (void*)NULL);
	init_action(&test.t_helpthreadact, SLEEP, UNLINK, 2, (void*)FILE1, (void*)NULL);
	execute_test(&test);
	
	
	init_test(&test, "5.1.1.5: link A to B, watch A, remove B", FILE1, 2, 1, NOTE_LINK, YES_EVENT);
	init_action(&(test.t_prep_actions[0]), NOSLEEP, CREAT, 2, (void*)FILE1, (void*)NULL);
	init_action(&(test.t_prep_actions[1]), NOSLEEP, HARDLINK, 2, (void*)FILE1, (void*)FILE2);
	init_action(&test.t_helpthreadact, SLEEP, UNLINK, 2, (void*)FILE2, (void*)NULL);
	init_action(&test.t_cleanup_actions[0], NOSLEEP, UNLINK, 2, (void*)FILE1, NULL);
	execute_test(&test);
	
	init_test(&test, "5.1.2: link() to a file", FILE1, 1, 2, NOTE_LINK, YES_EVENT);
	init_action(&(test.t_prep_actions[0]), NOSLEEP, CREAT, 2, (void*)FILE1, (void*)NULL);
	init_action(&test.t_helpthreadact, SLEEP, HARDLINK, 2, (void*)FILE1, (void*)FILE2);
	init_action(&test.t_cleanup_actions[0], NOSLEEP, UNLINK, 2, (void*)FILE1, NULL);
	init_action(&test.t_cleanup_actions[1], NOSLEEP, UNLINK, 2, (void*)FILE2, NULL);
	execute_test(&test);
	
	makepath(pathbuf, DIR1, DIR2);
	init_test(&test, "5.1.3: make one dir in another", DIR1, 1, 2, NOTE_LINK, YES_EVENT);
	init_action(&(test.t_prep_actions[0]), NOSLEEP, MKDIR, 2, (void*)DIR1, (void*)NULL);
	init_action(&test.t_helpthreadact, SLEEP, MKDIR, 2, (void*)pathbuf, (void*)NULL);
	init_action(&test.t_cleanup_actions[0], NOSLEEP, RMDIR, 2, (void*)pathbuf, NULL);
	init_action(&test.t_cleanup_actions[1], NOSLEEP, RMDIR, 2, (void*)DIR1, NULL);
	execute_test(&test);
	
	makepath(pathbuf, DIR1, DIR2);
	init_test(&test, "5.1.4: rmdir a dir from within another", DIR1, 2, 1, NOTE_LINK, YES_EVENT);
	init_action(&(test.t_prep_actions[0]), NOSLEEP, MKDIR, 2, (void*)DIR1, (void*)NULL);
	init_action(&(test.t_prep_actions[1]), NOSLEEP, MKDIR, 2, (void*)pathbuf, (void*)NULL);
	init_action(&test.t_helpthreadact, SLEEP, RMDIR, 2, (void*)pathbuf, (void*)NULL);
	init_action(&test.t_cleanup_actions[0], NOSLEEP, RMDIR, 2, (void*)DIR1, NULL);
	execute_test(&test);
	
	makepath(pathbuf, DIR1, DIR2);
	makepath(otherpathbuf, DIR1, DIR1);
	init_test(&test, "5.1.5: rename dir A over dir B inside dir C", DIR1, 3, 2, NOTE_LINK, YES_EVENT);
	init_action(&(test.t_prep_actions[0]), NOSLEEP, MKDIR, 2, (void*)DIR1, (void*)NULL);
	init_action(&(test.t_prep_actions[1]), NOSLEEP, MKDIR, 2, (void*)pathbuf, (void*)NULL);
	init_action(&(test.t_prep_actions[2]), NOSLEEP, MKDIR, 2, (void*)otherpathbuf, (void*)NULL);
	init_action(&test.t_helpthreadact, SLEEP, RENAME, 2, (void*)pathbuf, (void*)otherpathbuf);
	init_action(&test.t_cleanup_actions[0], NOSLEEP, RMDIR, 2, (void*)otherpathbuf, NULL);
	init_action(&test.t_cleanup_actions[1], NOSLEEP, RMDIR, 2, (void*)DIR1, NULL);
	execute_test(&test);
	
	LOG(1, stderr, "HFS bypasses hfs_makenode to create in target, so misses knote.\n");
	makepath(pathbuf, DIR1, DIR2);
	init_test(&test, "5.1.6: rename one dir into another", DIR1, 2, 2, NOTE_LINK, YES_EVENT);
	init_action(&(test.t_prep_actions[0]), NOSLEEP, MKDIR, 2, (void*)DIR1, (void*)NULL);
	init_action(&(test.t_prep_actions[1]), NOSLEEP, MKDIR, 2, (void*)DIR2, (void*)NULL);
	init_action(&test.t_helpthreadact, SLEEP, RENAME, 2, (void*)DIR2, (void*)pathbuf);
	init_action(&test.t_cleanup_actions[0], NOSLEEP, RMDIR, 2, (void*)pathbuf, NULL);
	init_action(&test.t_cleanup_actions[1], NOSLEEP, RMDIR, 2, (void*)DIR1, NULL);
	execute_test(&test);
	
	LOG(1, stderr, "HFS bypasses hfs_removedir to remove from source, so misses knote.\n");
	makepath(pathbuf, DIR1, DIR2);
	init_test(&test, "5.1.7: rename one dir out of another", DIR1, 2, 2, NOTE_LINK, YES_EVENT);
	init_action(&(test.t_prep_actions[0]), NOSLEEP, MKDIR, 2, (void*)DIR1, (void*)NULL);
	init_action(&(test.t_prep_actions[1]), NOSLEEP, MKDIR, 2, (void*)pathbuf, (void*)NULL);
	init_action(&test.t_helpthreadact, SLEEP, RENAME, 2, (void*)pathbuf, (void*)DIR2);
	init_action(&test.t_cleanup_actions[0], NOSLEEP, RMDIR, 2, (void*)DIR2, NULL);
	init_action(&test.t_cleanup_actions[1], NOSLEEP, RMDIR, 2, (void*)DIR1, NULL);
	execute_test(&test);
	
	init_test(&test, "5.1.8: rmdir a dir", DIR1, 1, 0, NOTE_LINK, YES_EVENT);
	init_action(&(test.t_prep_actions[0]), NOSLEEP, MKDIR, 2, (void*)DIR1, (void*)NULL);
	init_action(&test.t_helpthreadact, SLEEP, RMDIR, 2, (void*)DIR1, (void*)NULL);
	execute_test(&test);
	
	/* ============= NO EVENT SECTION ============== */
	makepath(pathbuf, DIR1, FILE1);
	init_test(&test, "5.2.1: make a file in a dir", DIR1, 1, 2, NOTE_LINK, NO_EVENT);
	init_action(&(test.t_prep_actions[0]), NOSLEEP, MKDIR, 2, (void*)DIR1, (void*)NULL);
	init_action(&test.t_helpthreadact, SLEEP, CREAT, 2, (void*)pathbuf, (void*)NULL);
	init_action(&test.t_cleanup_actions[0], NOSLEEP, UNLINK, 2, (void*)pathbuf, NULL);
	init_action(&test.t_cleanup_actions[1], NOSLEEP, RMDIR, 2, (void*)DIR1, NULL);
	execute_test(&test);
	
	makepath(pathbuf, DIR1, FILE1);
	init_test(&test, "5.2.2: unlink a file in a dir", DIR1, 2, 1, NOTE_LINK, NO_EVENT);
	init_action(&(test.t_prep_actions[0]), NOSLEEP, MKDIR, 2, (void*)DIR1, (void*)NULL);
	init_action(&(test.t_prep_actions[1]), NOSLEEP, CREAT, 2, (void*)pathbuf, (void*)NULL);
	init_action(&test.t_helpthreadact, SLEEP, UNLINK, 2, (void*)pathbuf, (void*)NULL);
	init_action(&test.t_cleanup_actions[0], NOSLEEP, RMDIR, 2, (void*)DIR1, NULL);
	execute_test(&test);
	
	makepath(pathbuf, DIR1, FILE1);
	makepath(otherpathbuf, DIR1, FILE2);
	init_test(&test, "5.2.3: rename a file within a dir", DIR1, 2, 2, NOTE_LINK, NO_EVENT);
	init_action(&(test.t_prep_actions[0]), NOSLEEP, MKDIR, 2, (void*)DIR1, (void*)NULL);
	init_action(&(test.t_prep_actions[1]), NOSLEEP, CREAT, 2, (void*)pathbuf, (void*)NULL);
	init_action(&test.t_helpthreadact, SLEEP, RENAME, 2, (void*)pathbuf, (void*)otherpathbuf);
	init_action(&test.t_cleanup_actions[0], NOSLEEP, UNLINK, 2, (void*)otherpathbuf, NULL);
	init_action(&test.t_cleanup_actions[1], NOSLEEP, RMDIR, 2, (void*)DIR1, NULL);
	execute_test(&test);
	
	makepath(pathbuf, DIR1, FILE1);
	init_test(&test, "5.2.4: rename a file into a dir", DIR1, 2, 2, NOTE_LINK, NO_EVENT);
	init_action(&(test.t_prep_actions[0]), NOSLEEP, MKDIR, 2, (void*)DIR1, (void*)NULL);
	init_action(&(test.t_prep_actions[1]), NOSLEEP, CREAT, 2, (void*)FILE1, (void*)NULL);
	init_action(&test.t_helpthreadact, SLEEP, RENAME, 2, (void*)FILE1, (void*)pathbuf);
	init_action(&test.t_cleanup_actions[0], NOSLEEP, UNLINK, 2, (void*)pathbuf, NULL);
	init_action(&test.t_cleanup_actions[1], NOSLEEP, RMDIR, 2, (void*)DIR1, NULL);
	execute_test(&test);
	
	makepath(pathbuf, DIR1, FILE1);
	init_test(&test, "5.2.5: make a symlink in a dir", DIR1, 1, 2, NOTE_LINK, NO_EVENT);
	init_action(&(test.t_prep_actions[0]), NOSLEEP, MKDIR, 2, (void*)DIR1, (void*)NULL);
	init_action(&test.t_helpthreadact, SLEEP, SYMLINK, 2, (void*)DOTDOT, (void*)pathbuf);
	init_action(&test.t_cleanup_actions[0], NOSLEEP, UNLINK, 2, (void*)pathbuf, NULL);
	init_action(&test.t_cleanup_actions[1], NOSLEEP, RMDIR, 2, (void*)DIR1, NULL);
	execute_test(&test);
	
	init_test(&test, "5.2.6: make a symlink to a dir", DIR1, 1, 2, NOTE_LINK, NO_EVENT);
	init_action(&(test.t_prep_actions[0]), NOSLEEP, MKDIR, 2, (void*)DIR1, (void*)NULL);
	init_action(&test.t_helpthreadact, SLEEP, SYMLINK, 2, (void*)DIR1, (void*)FILE1);
	init_action(&test.t_cleanup_actions[0], NOSLEEP, UNLINK, 2, (void*)FILE1, NULL);
	init_action(&test.t_cleanup_actions[1], NOSLEEP, RMDIR, 2, (void*)DIR1, NULL);
	execute_test(&test);
	
	init_test(&test, "5.2.7: make a symlink to a file", FILE1, 1, 2, NOTE_LINK, NO_EVENT);
	init_action(&(test.t_prep_actions[0]), NOSLEEP, CREAT, 2, (void*)FILE1, (void*)NULL);
	init_action(&test.t_helpthreadact, SLEEP, SYMLINK, 2, (void*)FILE1, (void*)FILE2);
	init_action(&test.t_cleanup_actions[0], NOSLEEP, UNLINK, 2, (void*)FILE2, NULL);
	init_action(&test.t_cleanup_actions[1], NOSLEEP, UNLINK, 2, (void*)FILE1, NULL);
	execute_test(&test);
}

void
run_note_rename_tests() 
{
	test_t test;
	
	init_test(&test, "6.1.1: rename a file", FILE1, 1, 1, NOTE_RENAME, YES_EVENT);
	init_action(&(test.t_prep_actions[0]), NOSLEEP, CREAT, 2, (void*)FILE1, (void*)NULL);
	init_action(&test.t_helpthreadact, SLEEP, RENAME, 2, (void*)FILE1, (void*)FILE2);
	init_action(&test.t_cleanup_actions[0], NOSLEEP, UNLINK, 2, (void*)FILE2, NULL);
	execute_test(&test);
	
	init_test(&test, "6.1.2: rename a dir", DIR1, 1, 1, NOTE_RENAME, YES_EVENT);
	init_action(&(test.t_prep_actions[0]), NOSLEEP, MKDIR, 2, (void*)DIR1, (void*)NULL);
	init_action(&test.t_helpthreadact, SLEEP, RENAME, 2, (void*)DIR1, (void*)DIR2);
	init_action(&test.t_cleanup_actions[0], NOSLEEP, RMDIR, 2, (void*)DIR2, NULL);
	execute_test(&test);
	
	init_test(&test, "6.1.2: rename one file over another", FILE1, 2, 1, NOTE_RENAME, YES_EVENT);
	init_action(&(test.t_prep_actions[0]), NOSLEEP, CREAT, 2, (void*)FILE1, (void*)NULL);
	init_action(&(test.t_prep_actions[1]), NOSLEEP, CREAT, 2, (void*)FILE2, (void*)NULL);
	init_action(&test.t_helpthreadact, SLEEP, RENAME, 2, (void*)FILE1, (void*)FILE2);
	init_action(&test.t_cleanup_actions[0], NOSLEEP, UNLINK, 2, (void*)FILE2, NULL);
	execute_test(&test);
	
	init_test(&test, "6.1.3: rename one dir over another", DIR1, 2, 1, NOTE_RENAME, YES_EVENT);
	init_action(&(test.t_prep_actions[0]), NOSLEEP, MKDIR, 2, (void*)DIR1, (void*)NULL);
	init_action(&(test.t_prep_actions[1]), NOSLEEP, MKDIR, 2, (void*)DIR2, (void*)NULL);
	init_action(&test.t_helpthreadact, SLEEP, RENAME, 2, (void*)DIR1, (void*)DIR2);
	init_action(&test.t_cleanup_actions[0], NOSLEEP, RMDIR, 2, (void*)DIR2, NULL);
	execute_test(&test);
	
	/* ========= NO EVENT SECTION =========== */
	
	init_test(&test, "6.2.1: unlink a file", FILE1, 1, 0, NOTE_RENAME, NO_EVENT);
	init_action(&(test.t_prep_actions[0]), NOSLEEP, CREAT, 2, (void*)FILE1, (void*)NULL);
	init_action(&test.t_helpthreadact, SLEEP, UNLINK, 2, (void*)FILE1, NULL);
	execute_test(&test);
	
	init_test(&test, "6.2.2: rmdir a dir", DIR1, 1, 0, NOTE_RENAME, NO_EVENT);
	init_action(&(test.t_prep_actions[0]), NOSLEEP, MKDIR, 2, (void*)DIR1, (void*)NULL);
	init_action(&test.t_helpthreadact, SLEEP, RMDIR, 2, (void*)DIR1, NULL);
	execute_test(&test);
	
	init_test(&test, "6.2.3: link() to a file", FILE1, 1, 2, NOTE_RENAME, NO_EVENT);
	init_action(&(test.t_prep_actions[0]), NOSLEEP, CREAT, 2, (void*)FILE1, (void*)NULL);
	init_action(&test.t_helpthreadact, SLEEP, HARDLINK, 2, (void*)FILE1, (void*)FILE2);
	init_action(&test.t_cleanup_actions[0], NOSLEEP, UNLINK, 2, (void*)FILE1, NULL);
	init_action(&test.t_cleanup_actions[1], NOSLEEP, UNLINK, 2, (void*)FILE2, NULL);
	execute_test(&test);
	
	init_test(&test, "6.2.4: rename one file over another: watch deceased", 
			  FILE2, 2, 1, NOTE_RENAME, NO_EVENT);
	init_action(&(test.t_prep_actions[0]), NOSLEEP, CREAT, 2, (void*)FILE1, (void*)NULL);
	init_action(&(test.t_prep_actions[1]), NOSLEEP, CREAT, 2, (void*)FILE2, (void*)NULL);
	init_action(&test.t_helpthreadact, SLEEP, RENAME, 2, (void*)FILE1, (void*)FILE2);
	init_action(&test.t_cleanup_actions[0], NOSLEEP, UNLINK, 2, (void*)FILE2, NULL);
	execute_test(&test);
	
	init_test(&test, "6.2.5: rename one dir over another: watch deceased", 
			  DIR2, 2, 1, NOTE_RENAME, NO_EVENT);
	init_action(&(test.t_prep_actions[0]), NOSLEEP, MKDIR, 2, (void*)DIR1, (void*)NULL);
	init_action(&(test.t_prep_actions[1]), NOSLEEP, MKDIR, 2, (void*)DIR2, (void*)NULL);
	init_action(&test.t_helpthreadact, SLEEP, RENAME, 2, (void*)DIR1, (void*)DIR2);
	init_action(&test.t_cleanup_actions[0], NOSLEEP, RMDIR, 2, (void*)DIR2, NULL);
	execute_test(&test);
	
	init_test(&test, "6.2.6: rename a file to itself", FILE1, 1, 1, NOTE_RENAME, NO_EVENT);
	init_action(&(test.t_prep_actions[0]), NOSLEEP, CREAT, 2, (void*)FILE1, (void*)NULL);
	init_action(&test.t_helpthreadact, SLEEP, RENAME, 2, (void*)FILE1, (void*)FILE1);
	init_action(&test.t_cleanup_actions[0], NOSLEEP, UNLINK, 2, (void*)FILE1, NULL);
	execute_test(&test);
	
	init_test(&test, "6.2.7: rename a dir to itself", DIR1, 1, 1, NOTE_RENAME, NO_EVENT);
	init_action(&(test.t_prep_actions[0]), NOSLEEP, MKDIR, 2, (void*)DIR1, (void*)NULL);
	init_action(&test.t_helpthreadact, SLEEP, RENAME, 2, (void*)DIR1, (void*)DIR1);
	init_action(&test.t_cleanup_actions[0], NOSLEEP, RMDIR, 2, (void*)DIR1, NULL);
	execute_test(&test);
}

void 
run_note_revoke_tests() 
{
	test_t test;
	init_test(&test, "7.1.1: revoke file", FILE1, 1, 1, NOTE_REVOKE, YES_EVENT);
	init_action(&(test.t_prep_actions[0]), NOSLEEP, CREAT, 1, (void*)FILE1);
	init_action(&test.t_helpthreadact, SLEEP, REVOKE, 1, (void*)FILE1);
	init_action(&(test.t_cleanup_actions[0]), NOSLEEP, UNLINK, 1, (void*)FILE1);
	execute_test(&test);
	
	init_test(&test, "7.2.1: delete file", FILE1, 1, 0, NOTE_REVOKE, NO_EVENT);
	init_action(&(test.t_prep_actions[0]), NOSLEEP, CREAT, 1, (void*)FILE1);
	init_action(&test.t_helpthreadact, SLEEP, UNLINK, 1, (void*)FILE1);
	execute_test(&test);
}


void
run_evfilt_read_tests() 
{
	test_t test;
	init_test(&test, "8.1.1: how much data in file of length LENGTHEN_SIZE?", FILE1, 2, 1, EVFILT_READ, LENGTHEN_SIZE);
	init_action(&(test.t_prep_actions[0]), NOSLEEP, CREAT, 2, (void*)FILE1, (void*)NULL);
	init_action(&(test.t_prep_actions[1]), NOSLEEP, LENGTHEN, 2, (void*)FILE1, (void*)NULL);
	init_action(&test.t_helpthreadact, SLEEP, NOTHING, 0);
	init_action(&test.t_cleanup_actions[0], NOSLEEP, UNLINK, 2, (void*)FILE1, NULL);
	execute_test(&test);
	
	init_test(&test, "8.1.2: block, then write to file", FILE1, 2, 1, EVFILT_READ, strlen(TEST_STRING));
	init_action(&(test.t_prep_actions[0]), NOSLEEP, CREAT, 1, (void*)FILE1);
	init_action(&(test.t_prep_actions[1]), NOSLEEP, TRUNC, 1, (void*)FILE1);
	init_action(&test.t_helpthreadact, SLEEP, WRITE, 1, (void*)FILE1);
	init_action(&test.t_cleanup_actions[0], NOSLEEP, UNLINK, 2, (void*)FILE1, NULL);
	execute_test(&test);
	
	init_test(&test, "8.1.3: block, then extend", FILE1, 2, 1, EVFILT_READ, LENGTHEN_SIZE);
	init_action(&(test.t_prep_actions[0]), NOSLEEP, CREAT, 1, (void*)FILE1);
	init_action(&(test.t_prep_actions[1]), NOSLEEP, TRUNC, 1, (void*)FILE1);
	init_action(&test.t_helpthreadact, SLEEP, LENGTHEN, 1, (void*)FILE1);
	init_action(&test.t_cleanup_actions[0], NOSLEEP, UNLINK, 2, (void*)FILE1, NULL);
	execute_test(&test);
	
	init_test(&test, "8.1.4: block, then seek to beginning", FILE1, 2, 1, EVFILT_READ, strlen(TEST_STRING));
	init_action(&(test.t_prep_actions[0]), NOSLEEP, CREAT, 1, (void*)FILE1);
	init_action(&(test.t_prep_actions[1]), NOSLEEP, WRITE, 1, (void*)FILE1);
	test.t_read_to_end_first = 1; /* hack means that we've gotten to EOF before we block */
	init_action(&test.t_helpthreadact, SLEEP, LSEEK, 1, (void*)0);
	init_action(&test.t_cleanup_actions[0], NOSLEEP, UNLINK, 2, (void*)FILE1, NULL);
	execute_test(&test);
	
	
	init_test(&test, "8.1.5: block, then write to fifo", FILE1, 1, 1, EVFILT_READ, strlen(TEST_STRING));
	init_action(&(test.t_prep_actions[0]), NOSLEEP, MKFIFO, 1, (void*)FILE1);
	test.t_file_is_fifo = 1;
	init_action(&test.t_helpthreadact, SLEEP, WRITE, 1, (void*)FILE1);
	init_action(&test.t_cleanup_actions[0], NOSLEEP, UNLINK, 2, (void*)FILE1, NULL);
	execute_test(&test);
	
	/* No result section... */
	init_test(&test, "8.2.1: just rename", FILE1, 2, 1, EVFILT_READ, NO_EVENT);
	init_action(&(test.t_prep_actions[0]), NOSLEEP, CREAT, 1, (void*)FILE1);
	init_action(&(test.t_prep_actions[1]), NOSLEEP, TRUNC, 1, (void*)FILE1);
	init_action(&test.t_helpthreadact, SLEEP, RENAME, 2, (void*)FILE1, (void*)FILE2);
	init_action(&test.t_cleanup_actions[0], NOSLEEP, UNLINK, 2, (void*)FILE2, NULL);
	execute_test(&test);
	
	init_test(&test, "8.2.2: delete file", FILE1, 2, 0, EVFILT_READ, NO_EVENT);
	init_action(&(test.t_prep_actions[0]), NOSLEEP, CREAT, 1, (void*)FILE1);
	init_action(&(test.t_prep_actions[1]), NOSLEEP, TRUNC, 1, (void*)FILE1);
	init_action(&test.t_helpthreadact, SLEEP, UNLINK, 1, (void*)FILE1);
	execute_test(&test);
	
	init_test(&test, "8.2.3: write to beginning", FILE1, 2, 1, EVFILT_READ, NO_EVENT);
	init_action(&(test.t_prep_actions[0]), NOSLEEP, CREAT, 1, (void*)FILE1);
	init_action(&(test.t_prep_actions[1]), NOSLEEP, WRITE, 1, (void*)FILE1);
	test.t_read_to_end_first = 1; /* hack means that we've gotten to EOF before we block */
	init_action(&test.t_helpthreadact, SLEEP, WRITE, 1, (void*)FILE1);
	init_action(&test.t_cleanup_actions[0], NOSLEEP, UNLINK, 1, (void*)FILE1);
	execute_test(&test);
	
	init_test(&test, "8.1.4: block, then seek to current location", FILE1, 2, 1, EVFILT_READ, 0);
	init_action(&(test.t_prep_actions[0]), NOSLEEP, CREAT, 1, (void*)FILE1);
	init_action(&(test.t_prep_actions[1]), NOSLEEP, WRITE, 1, (void*)FILE1);
	test.t_read_to_end_first = 1; /* hack means that we've gotten to EOF before we block */
	init_action(&test.t_helpthreadact, SLEEP, LSEEK, 1, (void*)strlen(TEST_STRING));
	init_action(&test.t_cleanup_actions[0], NOSLEEP, UNLINK, 2, (void*)FILE1, NULL);
	execute_test(&test);
	
	init_test(&test, "8.2.5: trying to read from empty fifo", FILE1, 1, 1, EVFILT_READ, 0);
	init_action(&(test.t_prep_actions[0]), NOSLEEP, MKFIFO, 1, (void*)FILE1);
	test.t_file_is_fifo = 1;
	init_action(&test.t_helpthreadact, SLEEP, NOTHING, 1, (void*)0);
	init_action(&test.t_cleanup_actions[0], NOSLEEP, UNLINK, 2, (void*)FILE1, NULL);
	execute_test(&test);
	
}



void*
read_from_fd(void *arg)
{
	char buf[50];
	int fd = (int) arg;
	sleep(2);
	return (void*) read(fd, buf, sizeof(buf));
}

void*
write_to_fd(void *arg)
{
	char buf[50];
	int fd = (int) arg;
	sleep(2);
	return (void*) write(fd, buf, sizeof(buf));
}

/*
 * We don't (in principle) support EVFILT_WRITE for vnodes; thusly, no tests here
 */
void 
run_evfilt_write_tests()
{
	
	test_t test;
	init_test(&test, "9.1.1: how much space in empty fifo?", FILE1, 1, 1, EVFILT_WRITE, FIFO_SPACE);
	init_action(&(test.t_prep_actions[0]), NOSLEEP, MKFIFO, 1, (void*)FILE1, (void*)NULL);
	test.t_file_is_fifo = 1;
	init_action(&test.t_helpthreadact, SLEEP, NOTHING, 0);
	init_action(&test.t_cleanup_actions[0], NOSLEEP, UNLINK, 2, (void*)FILE1, NULL);
	execute_test(&test);
	
	init_test(&test, "9.1.2: how much space in slightly written fifo?", FILE1, 1, 1, EVFILT_WRITE, FIFO_SPACE - strlen(TEST_STRING));
	init_action(&(test.t_prep_actions[0]), NOSLEEP, MKFIFO, 1, (void*)FILE1, (void*)NULL);
	test.t_file_is_fifo = 1;
	test.t_write_some_data = 1;
	init_action(&(test.t_helpthreadact), NOSLEEP, NOTHING, 0);
	init_action(&test.t_cleanup_actions[0], NOSLEEP, UNLINK, 2, (void*)FILE1, NULL);
	execute_test(&test);
	
	init_test(&test, "9.2.1: how much space in a full fifo?", FILE1, 1, 1, EVFILT_WRITE, 0);
	init_action(&(test.t_prep_actions[0]), NOSLEEP, MKFIFO, 1, (void*)FILE1, (void*)NULL);
	test.t_file_is_fifo = 1;
	test.t_extra_sleep_hack = 1;
	init_action(&(test.t_helpthreadact), NOSLEEP, FILLFD, 1, (void*)FILE1, (void*)NULL);
	init_action(&test.t_cleanup_actions[0], NOSLEEP, UNLINK, 2, (void*)FILE1, NULL);
	execute_test(&test);
}

void
run_poll_tests()
{
	test_t test;
	init_poll_test(&test, "10.1.1: does poll say I can write a regular file?", FILE1, 1, 1, POLLWRNORM, 1);
	init_action(&(test.t_prep_actions[0]), NOSLEEP, CREAT, 1, (void*)FILE1, (void*)NULL);
	init_action(&test.t_helpthreadact, SLEEP, NOTHING, 0);
	init_action(&test.t_cleanup_actions[0], NOSLEEP, UNLINK, 2, (void*)FILE1, NULL);
	execute_test(&test);
	
	init_poll_test(&test, "10.1.2: does poll say I can write an empty FIFO?", FILE1, 1, 1, POLLWRNORM, 1);
	init_action(&(test.t_prep_actions[0]), NOSLEEP, MKFIFO, 1, (void*)FILE1, (void*)NULL);
	test.t_file_is_fifo = 1;
	init_action(&test.t_helpthreadact, SLEEP, NOTHING, 0);
	init_action(&test.t_cleanup_actions[0], NOSLEEP, UNLINK, 2, (void*)FILE1, NULL);
	execute_test(&test);
	
	init_poll_test(&test, "10.1.3: does poll say I can read a nonempty FIFO?", FILE1, 1, 1, POLLRDNORM, 1);
	init_action(&(test.t_prep_actions[0]), NOSLEEP, MKFIFO, 1, (void*)FILE1, (void*)NULL);
	test.t_file_is_fifo = 1;
	test.t_write_some_data = 1;
	init_action(&test.t_helpthreadact, SLEEP, NOTHING, 0);
	init_action(&test.t_cleanup_actions[0], NOSLEEP, UNLINK, 2, (void*)FILE1, NULL);
	execute_test(&test);
	
	init_poll_test(&test, "10.1.4: does poll say I can read a nonempty regular file?", FILE1, 2, 1, POLLRDNORM, 1);
	init_action(&(test.t_prep_actions[0]), NOSLEEP, CREAT, 1, (void*)FILE1, (void*)NULL);
	init_action(&(test.t_prep_actions[1]), NOSLEEP, LENGTHEN, 1, (void*)FILE1, (void*)NULL);
	init_action(&test.t_helpthreadact, SLEEP, NOTHING, 0);
	init_action(&test.t_cleanup_actions[0], NOSLEEP, UNLINK, 2, (void*)FILE1, NULL);
	execute_test(&test);
	
	init_poll_test(&test, "10.1.5: does poll say I can read an empty file?", FILE1, 1, 1, POLLRDNORM, 1);
	init_action(&(test.t_prep_actions[0]), NOSLEEP, CREAT, 1, (void*)FILE1, (void*)NULL);
	init_action(&test.t_helpthreadact, SLEEP, NOTHING, 0);
	init_action(&test.t_cleanup_actions[0], NOSLEEP, UNLINK, 2, (void*)FILE1, NULL);
	execute_test(&test);
	
	
	
	
	init_poll_test(&test, "10.2.2: does poll say I can read an empty FIFO?", FILE1, 1, 1, POLLRDNORM, 0);
	init_action(&(test.t_prep_actions[0]), NOSLEEP, MKFIFO, 1, (void*)FILE1, (void*)NULL);
	test.t_file_is_fifo = 1;
	init_action(&test.t_helpthreadact, SLEEP, NOTHING, 0);
	init_action(&test.t_cleanup_actions[0], NOSLEEP, UNLINK, 2, (void*)FILE1, NULL);
	execute_test(&test);
	
	init_poll_test(&test, "10.2.3: does poll say I can write a full FIFO?", FILE1, 1, 1, POLLWRNORM, 0);
	init_action(&(test.t_prep_actions[0]), NOSLEEP, MKFIFO, 1, (void*)FILE1, (void*)NULL);
	test.t_file_is_fifo = 1;
	test.t_extra_sleep_hack = 1;
	init_action(&(test.t_helpthreadact), NOSLEEP, FILLFD, 1, (void*)FILE1, (void*)NULL);
	init_action(&test.t_cleanup_actions[0], NOSLEEP, UNLINK, 2, (void*)FILE1, NULL);
	execute_test(&test);
}

void 
run_all_tests() 
{
	run_note_delete_tests();
	run_note_write_tests();
	run_note_extend_tests();
	run_note_attrib_tests();
	run_note_link_tests();
	run_note_rename_tests();
#if 0
	run_note_revoke_tests(); /* Can no longer revoke a regular file--need an unmount test */
#endif /* 0 */
	run_evfilt_read_tests();
	run_evfilt_write_tests();
	run_poll_tests();
}

int 
main(int argc, char **argv) 
{
	char *which = NULL;
	if (argc > 1) {
		which = argv[1];
	}
	
	if ((!which) || (strcmp(which, "all") == 0))
		run_all_tests();
	else if (strcmp(which, "delete") == 0) 
		run_note_delete_tests();
	else if (strcmp(which, "write") == 0)
		run_note_write_tests();
	else if (strcmp(which, "extend") == 0)
		run_note_extend_tests();
	else if (strcmp(which, "attrib") == 0)
		run_note_attrib_tests();
	else if (strcmp(which, "link") == 0)
		run_note_link_tests();
	else if (strcmp(which, "rename") == 0)
		run_note_rename_tests();
	else if (strcmp(which, "revoke") == 0)
		run_note_revoke_tests();
	else if (strcmp(which, "evfiltread") == 0)
		run_evfilt_read_tests();
	else if (strcmp(which, "evfiltwrite") == 0)
		run_evfilt_write_tests();
	else if (strcmp(which, "poll") == 0)
		run_poll_tests();
	else {
		fprintf(stderr, "Valid options are:\n\tdelete, write, extend,"
				"attrib, link, rename, revoke, evfiltread, fifo, all, evfiltwrite<none>\n");
		exit(1);
	}
	return 0;
}

