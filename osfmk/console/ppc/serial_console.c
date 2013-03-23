/*
 * Copyright (c) 2000-2004 Apple Computer, Inc. All rights reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
 * 
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this
 * file.
 * 
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 * 
 * @APPLE_LICENSE_HEADER_END@
 */
/*
 * @OSF_COPYRIGHT@
 */
/*
 * @APPLE_FREE_COPYRIGHT@
 */

#include <mach_kdb.h>
#include <platforms.h>
#include <serial_console_default.h>

#include <kern/spl.h>
#include <machine/machparam.h>		/* spl definitions */
#include <types.h>
#include <console/video_console.h>
#include <kern/kalloc.h>
#include <kern/thread.h>
#include <ppc/misc_protos.h>
#include <ppc/serial_io.h>
#include <kern/cpu_number.h>
#include <ppc/Firmware.h>
#include <ppc/proc_reg.h>
#include <ppc/cpu_internal.h>
#include <ppc/exception.h>
#include <pexpert/pexpert.h>

/*
 * A machine MUST have a console.  In our case
 * things are a little complicated by the graphic
 * display: people expect it to be their "console",
 * but we'd like to be able to live without it.
 * This is not to be confused with the "rconsole" thing:
 * that just duplicates the console I/O to
 * another place (for debugging/logging purposes).
 */

const int console_unit = 0;
const int console_chan_default = CONSOLE_PORT;
#define console_chan (console_chan_default) /* ^ cpu_number()) */

#define OPS(putc, getc, nosplputc, nosplgetc) putc, getc

const struct console_ops {
	int	(*putc)(int, int, int);
	int	(*getc)(int, int, boolean_t, boolean_t);
} cons_ops[] = {
#define SCC_CONS_OPS 0
	{OPS(scc_putc, scc_getc, no_spl_scputc, no_spl_scgetc)},
#define VC_CONS_OPS 1
	{OPS(vcputc, vcgetc, no_spl_vcputc, no_spl_vcgetc)},
};
#define NCONSOPS (sizeof cons_ops / sizeof cons_ops[0])

#if SERIAL_CONSOLE_DEFAULT
#define CONS_OPS SCC_CONS_OPS
#define CONS_NAME "com"
#else
#define CONS_OPS VC_CONS_OPS
#define CONS_NAME "vc"
#endif

#define MP_SAFE_CONSOLE 1	/* Set this to 1 to allow more than 1 processor to print at once */
#if MP_SAFE_CONSOLE

struct ppcbfr {													/* Controls multiple processor output */
	unsigned int 	pos;										/* Current position in buffer */
	unsigned int	noprompt;									/* Set if we skip the prompt */
	unsigned int	echo;										/* Control character echoing */
	char			buffer[256];								/* Fairly big buffer */	
};
typedef struct ppcbfr ppcbfr_t;

ppcbfr_t cbfr_boot_cpu;											/* Get one for boot cpu */
volatile unsigned int cbfpend;									/* A buffer is pending output */
volatile unsigned int sconowner=-1;								/* Mark who's actually writing */

#endif


unsigned int cons_ops_index = CONS_OPS;
unsigned int killprint = 0;
unsigned int debcnputc = 0;
extern unsigned int	mappingdeb0;
extern int debugger_cpu;

void *console_per_proc_alloc(boolean_t boot_processor)
{
	ppcbfr_t  *cbfr_cpu;

	if (boot_processor)
		cbfr_cpu = &cbfr_boot_cpu;
	else {
		cbfr_cpu = (ppcbfr_t *)kalloc(sizeof(ppcbfr_t));
		if (cbfr_cpu == (ppcbfr_t *)NULL)
			return (void *)NULL;
	}
	bzero((char *)cbfr_cpu, sizeof(ppcbfr_t));
	return (void *)cbfr_cpu;
}

void console_per_proc_free(void *per_proc_cbfr)
{
	if (per_proc_cbfr == (void *)&cbfr_boot_cpu)
		return;
	else
		kfree(per_proc_cbfr, sizeof(ppcbfr_t));
}


static void _cnputc(char c)
{
	cons_ops[cons_ops_index].putc(console_unit, console_chan, c);
}

void cnputcusr(char c) {										/* Echo input character directly */
	struct per_proc_info	*procinfo;
	spl_t 					s;
	
	s=splhigh();
	procinfo = getPerProc();

	hw_atomic_add(&(procinfo->debugger_holdoff), 1);			/* Don't allow debugger entry just now (this is a HACK) */

	_cnputc( c);												/* Echo the character */
	if(c=='\n') _cnputc( '\r');									/* Add a return if we had a new line */

	hw_atomic_sub(&(procinfo->debugger_holdoff), 1);			/* Don't allow debugger entry just now (this is a HACK) */
	splx(s);
	return;
}

void
cnputc(char c)
{
	unsigned int				oldpend, i, cpu, ourbit, sccpu;
	struct per_proc_info		*procinfo;
	ppcbfr_t					*cbfr, *cbfr_cpu;
	spl_t 						s;
		
#if MP_SAFE_CONSOLE

/*
 *		Handle multiple CPU console output.
 *		Note: this thing has gotten god-awful complicated.  We need a better way.
 */
 	

	if(killprint) {		
		return;													/* If printing is disabled, bail... */
	}	
	
	s=splhigh();												/* Don't bother me */
	procinfo = getPerProc();
	cpu = procinfo->cpu_number;
	cbfr = procinfo->pp_cbfr;

	hw_atomic_add(&(procinfo->debugger_holdoff), 1);			/* Don't allow debugger entry just now (this is a HACK) */

	ourbit = 1 << cpu;											/* Make a mask for just us */
	if(debugger_cpu != -1) {									/* Are we in the debugger with empty buffers? */
	
		while(sconowner != cpu) {								/* Anyone but us? */
			hw_compare_and_store(-1, cpu, (unsigned int *)&sconowner);	/* Try to mark it for us if idle */
		}
	
		_cnputc( c);											/* Yeah, just write it */
		if(c=='\n')												/* Did we just write a new line? */
			_cnputc( '\r');										/* Yeah, just add a return */
			
		sconowner=-1;											/* Mark it idle */	
		hw_atomic_sub(&(procinfo->debugger_holdoff), 1);			/* Don't allow debugger entry just now (this is a HACK) */
		
		splx(s);
		return;													/* Leave... */
	}

	
	while(ourbit&cbfpend);										/* We aren't "double buffered," so we'll just wait until the buffers are written */
	isync();													/* Just in case we had to wait */
	
	if(c) {														/* If the character is not null */
		cbfr->buffer[cbfr->pos]=c;							/* Fill in the buffer for our CPU */
		cbfr->pos++;										/* Up the count */
		if(cbfr->pos > 253) {								/* Is the buffer full? */
			cbfr->buffer[254]='\n';							/* Yeah, set the second to last as a LF */
			cbfr->buffer[255]='\r';							/* And the last to a CR */
			cbfr->pos=256;									/* Push the buffer to the end */
			c='\r';												/* Set character to a CR */
		}
	}
	
	if(c == '\n') {												/* Are we finishing a line? */
		cbfr->buffer[cbfr->pos]='\r';							/* And the last to a CR */
		cbfr->pos++;											/* Up the count */
		c='\r';													/* Set character to a CR */
	}

#if 1
	if(cbfr->echo == 1) {										/* Did we hit an escape last time? */
		if(c == 'K') {											/* Is it a partial clear? */
			cbfr->echo = 2;										/* Yes, enter echo mode */
		}
		else cbfr->echo = 0;									/* Otherwise reset escape */
	}
	else if(cbfr->echo == 0) {									/* Not in escape sequence, see if we should enter */
		cbfr->echo = 1;											/* Set that we are in escape sequence */
	}
#endif

	if((c == 0x00) || (c == '\r') || (cbfr->echo == 2)) {		/* Try to push out all buffers if we see CR or null */
				
		while(1) {												/* Loop until we see who's doing this */
			oldpend=cbfpend;									/* Get the currentest pending buffer flags */
			if(hw_compare_and_store(oldpend, oldpend|ourbit, (unsigned int *)&cbfpend))	/* Swap ours on if no change */
				break;											/* Bail the loop if it worked */
		}
		
		if(!hw_compare_and_store(-1, cpu, (unsigned int *)&sconowner)) {	/* See if someone else has this, and take it if not */
			procinfo->debugger_holdoff = 0;						/* Allow debugger entry (this is a HACK) */
			splx(s);											/* Let's take some 'rupts now */
			return;												/* We leave here, 'cause another processor is already writing the buffers */
		}
				
		while(1) {												/* Loop to dump out all of the finished buffers */
			oldpend=cbfpend;									/* Get the most current finished buffers */
			for(sccpu=0; sccpu<real_ncpus; sccpu++) {				/* Cycle through all CPUs buffers */
				if ((PerProcTable[sccpu].ppe_vaddr == 0)
				    || (PerProcTable[sccpu].ppe_vaddr->pp_cbfr == 0))
					continue;

				cbfr_cpu = PerProcTable[sccpu].ppe_vaddr->pp_cbfr;
				
				if(oldpend&(1<<sccpu)) {						/* Does this guy have a buffer to do? */

#if 0
					if(!cbfr_cpu->noprompt) {					/* Don't prompt if there was not CR before */
						_cnputc( '{');	/* Mark CPU number */
						_cnputc( '0'+sccpu);	/* Mark CPU number */
						_cnputc( '.');	/* (TEST/DEBUG) */
						_cnputc( '0'+cpu);	/* (TEST/DEBUG) */
						_cnputc( '}');	/* Mark CPU number */
						_cnputc( ' ');	/* Mark CPU number */
					}
#endif
					
					for(i=0; i<cbfr_cpu->pos; i++) {				/* Do the whole buffer */
						_cnputc(cbfr_cpu->buffer[i]);	 			/* Write it */
					}
					
					if(cbfr_cpu->buffer[cbfr_cpu->pos-1]!='\r') {	/* Was the last character a return? */
						cbfr_cpu->noprompt = 1;						/* Remember not to prompt */
					}
					else {											/* Last was a return */
						cbfr_cpu->noprompt = 0;						/* Otherwise remember to prompt */
						cbfr_cpu->echo = 0;							/* And clear echo */
					}
						
					cbfr_cpu->pos=0;								/* Reset the buffer pointer */
		
					while(!hw_compare_and_store(cbfpend, cbfpend&~(1<<sccpu), (unsigned int *)&cbfpend));	/* Swap it off */
				}
			}
			sconowner=-1;										/* Set the writer to idle */
			sync();												/* Insure that everything's done */
			if(hw_compare_and_store(0, 0, (unsigned int *)&cbfpend)) break;	/* If there are no new buffers, we are done... */
			if(!hw_compare_and_store(-1, cpu, (unsigned int *)&sconowner)) break;	/* If this isn't idle anymore, we're done */
	
		}
	}
	hw_atomic_sub(&(procinfo->debugger_holdoff), 1);					/* Don't allow debugger entry just now (this is a HACK) */
	splx(s);													/* Let's take some 'rupts now */

#else  /* MP_SAFE_CONSOLE */
	_cnputc( c);
	if (c == '\n')
		_cnputc('\r');
#endif  /* MP_SAFE_CONSOLE */

}

int
cngetc()
{
	return cons_ops[cons_ops_index].getc(console_unit, console_chan,
					     TRUE, FALSE);
}

int
cnmaygetc()
{
	return cons_ops[cons_ops_index].getc(console_unit, console_chan,
					     FALSE, FALSE);
}

boolean_t console_is_serial()
{
	return cons_ops_index == SCC_CONS_OPS;
}

int
switch_to_video_console()
{
	int old_cons_ops = cons_ops_index;
	cons_ops_index = VC_CONS_OPS;
	return old_cons_ops;
}

int
switch_to_serial_console()
{
	int old_cons_ops = cons_ops_index;
	cons_ops_index = SCC_CONS_OPS;
	return old_cons_ops;
}

/* The switch_to_{video,serial,kgdb}_console functions return a cookie that
   can be used to restore the console to whatever it was before, in the
   same way that splwhatever() and splx() work.  */
void
switch_to_old_console(int old_console)
{
	static boolean_t squawked;
	unsigned int ops = old_console;

	if (ops >= NCONSOPS && !squawked) {
		squawked = TRUE;
		printf("switch_to_old_console: unknown ops %d\n", ops);
	} else
		cons_ops_index = ops;
}


int
vcgetc(__unused int l, 
       __unused int u, 
       __unused boolean_t wait, 
       __unused boolean_t raw)
{
	char c;

	if( 0 == (*PE_poll_input)( 0, &c))
		return( c);
	else
		return( 0);
}
