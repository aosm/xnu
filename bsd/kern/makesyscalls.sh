#! /bin/sh -
#	@(#)makesyscalls.sh	8.1 (Berkeley) 6/10/93
# $FreeBSD: src/sys/kern/makesyscalls.sh,v 1.60 2003/04/01 01:12:24 jeff Exp $
#
# Copyright (c) 2006 Apple Computer, Inc. All rights reserved.
#
# @APPLE_LICENSE_OSREFERENCE_HEADER_START@
# 
# This file contains Original Code and/or Modifications of Original Code 
# as defined in and that are subject to the Apple Public Source License 
# Version 2.0 (the 'License'). You may not use this file except in 
# compliance with the License.  The rights granted to you under the 
# License may not be used to create, or enable the creation or 
# redistribution of, unlawful or unlicensed copies of an Apple operating 
# system, or to circumvent, violate, or enable the circumvention or 
# violation of, any terms of an Apple operating system software license 
# agreement.
#
# Please obtain a copy of the License at 
# http://www.opensource.apple.com/apsl/ and read it before using this 
# file.
#
# The Original Code and all software distributed under the License are 
# distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER 
# EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES, 
# INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY, 
# FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT. 
# Please see the License for the specific language governing rights and 
# limitations under the License.
#
# @APPLE_LICENSE_OSREFERENCE_HEADER_END@
#

set -e

# output files:
syscallnamesfile="syscalls.c"
sysprotofile="../sys/sysproto.h"
sysproto_h=_SYS_SYSPROTO_H_
syshdrfile="../sys/syscall.h"
syscall_h=_SYS_SYSCALL_H_
syscalltablefile="init_sysent.c"
syscallprefix="SYS_"
switchname="sysent"
namesname="syscallnames"

# tmp files:
syslegal="sysent.syslegal.$$"
sysent="sysent.switch.$$"
sysinc="sysinc.switch.$$"
sysarg="sysarg.switch.$$"
sysprotoend="sysprotoend.$$"
syscallnamestempfile="syscallnamesfile.$$"
syshdrtempfile="syshdrtempfile.$$"

trap "rm $syslegal $sysent $sysinc $sysarg $sysprotoend $syscallnamestempfile $syshdrtempfile" 0

touch $syslegal $sysent $sysinc $sysarg $sysprotoend $syscallnamestempfile $syshdrtempfile

case $# in
    0)	echo "usage: $0 input-file <config-file>" 1>&2
	exit 1
	;;
esac

if [ -n "$2" -a -f "$2" ]; then
	. $2
fi

sed -e '
s/\$//g
:join
	/\\$/{a\

	N
	s/\\\n//
	b join
	}
2,${
	/^#/!s/\([{}()*,]\)/ \1 /g
}
' < $1 | awk "
	BEGIN {
		syslegal = \"$syslegal\"
		sysprotofile = \"$sysprotofile\"
		sysprotoend = \"$sysprotoend\"
		sysproto_h = \"$sysproto_h\"
		syscall_h = \"$syscall_h\"
		sysent = \"$sysent\"
		syscalltablefile = \"$syscalltablefile\"
		sysinc = \"$sysinc\"
		sysarg = \"$sysarg\"
		syscallnamesfile = \"$syscallnamesfile\"
		syscallnamestempfile = \"$syscallnamestempfile\"
		syshdrfile = \"$syshdrfile\"
		syshdrtempfile = \"$syshdrtempfile\"
		syscallprefix = \"$syscallprefix\"
		switchname = \"$switchname\"
		namesname = \"$namesname\"
		infile = \"$1\"
		"'

		printf "/*\n" > syslegal
		printf " * Copyright (c) 2006 Apple Computer, Inc. All rights reserved.\n" > syslegal
		printf " * \n" > syslegal
		printf " * @APPLE_LICENSE_OSREFERENCE_HEADER_START@ \n" > syslegal
		printf " * \n" > syslegal
		printf " * This file contains Original Code and/or Modifications of Original Code \n" > syslegal
		printf " * as defined in and that are subject to the Apple Public Source License \n" > syslegal
		printf " * Version 2.0 (the \"License\"). You may not use this file except in \n" > syslegal
		printf " * compliance with the License.  The rights granted to you under the \n" > syslegal
		printf " * License may not be used to create, or enable the creation or \n" > syslegal
		printf " * redistribution of, unlawful or unlicensed copies of an Apple operating \n" > syslegal
		printf " * system, or to circumvent, violate, or enable the circumvention or \n" > syslegal
		printf " * violation of, any terms of an Apple operating system software license \n" > syslegal
		printf " * agreement. \n" > syslegal
		printf " * \n" > syslegal
		printf " * Please obtain a copy of the License at \n" > syslegal
		printf " * http://www.opensource.apple.com/apsl/ and read it before using this \n" > syslegal
		printf " * file. \n" > syslegal
		printf " * \n" > syslegal
		printf " * The Original Code and all software distributed under the License are \n" > syslegal
		printf " * distributed on an \"AS IS\" basis, WITHOUT WARRANTY OF ANY KIND, EITHER \n" > syslegal
		printf " * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES, \n" > syslegal
		printf " * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY, \n" > syslegal
		printf " * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT. \n" > syslegal
		printf " * Please see the License for the specific language governing rights and \n" > syslegal
		printf " * limitations under the License. \n" > syslegal
		printf " * \n" > syslegal
		printf " * @APPLE_LICENSE_OSREFERENCE_HEADER_END@ \n" > syslegal
		printf " * \n" > syslegal
		printf " * \n" > syslegal
		printf " * System call switch table.\n *\n" > syslegal
		printf " * DO NOT EDIT-- this file is automatically generated.\n" > syslegal
		printf " * created from %s\n */\n\n", infile > syslegal
	}
	NR == 1 {
		printf "\n/* The casts are bogus but will do for now. */\n" > sysent
		printf "__private_extern__ struct sysent %s[] = {\n",switchname > sysent

		printf "#ifndef %s\n", sysproto_h > sysarg
		printf "#define\t%s\n\n", sysproto_h > sysarg
		printf "#ifndef %s\n", syscall_h > syshdrtempfile
		printf "#define\t%s\n\n", syscall_h > syshdrtempfile
		printf "#include <sys/appleapiopts.h>\n" > syshdrtempfile
		printf "#ifdef __APPLE_API_PRIVATE\n" > syshdrtempfile
		printf "#include <sys/appleapiopts.h>\n" > sysarg
		printf "#include <sys/cdefs.h>\n" > sysarg
		printf "#include <sys/mount_internal.h>\n" > sysarg
		printf "#include <sys/types.h>\n" > sysarg
		printf "#include <sys/sem_internal.h>\n" > sysarg
		printf "#include <sys/semaphore.h>\n" > sysarg
		printf "#include <sys/wait.h>\n" > sysarg
		printf "#include <mach/shared_memory_server.h>\n" > sysarg
		printf "\n#ifdef KERNEL\n" > sysarg
		printf "#ifdef __APPLE_API_PRIVATE\n" > sysarg
		printf "#ifdef __ppc__\n" > sysarg
		printf "#define\tPAD_(t)\t(sizeof(uint64_t) <= sizeof(t) \\\n " > sysarg
		printf "\t\t? 0 : sizeof(uint64_t) - sizeof(t))\n" > sysarg
		printf "#else\n" > sysarg
		printf "#define\tPAD_(t)\t(sizeof(register_t) <= sizeof(t) \\\n " > sysarg
		printf "\t\t? 0 : sizeof(register_t) - sizeof(t))\n" > sysarg
		printf "#endif\n" > sysarg
		printf "#if BYTE_ORDER == LITTLE_ENDIAN\n"> sysarg
		printf "#define\tPADL_(t)\t0\n" > sysarg
		printf "#define\tPADR_(t)\tPAD_(t)\n" > sysarg
		printf "#else\n" > sysarg
		printf "#define\tPADL_(t)\tPAD_(t)\n" > sysarg
		printf "#define\tPADR_(t)\t0\n" > sysarg
		printf "#endif\n" > sysarg
		printf "\n__BEGIN_DECLS\n" > sysarg
		printf "#ifndef __MUNGE_ONCE\n" > sysarg
		printf "#define __MUNGE_ONCE\n" > sysarg
		printf "#ifdef __ppc__\n" > sysarg
		printf "void munge_w(const void *, void *);  \n" > sysarg
		printf "void munge_ww(const void *, void *);  \n" > sysarg
		printf "void munge_www(const void *, void *);  \n" > sysarg
		printf "void munge_wwww(const void *, void *);  \n" > sysarg
		printf "void munge_wwwww(const void *, void *);  \n" > sysarg
		printf "void munge_wwwwww(const void *, void *);  \n" > sysarg
		printf "void munge_wwwwwww(const void *, void *);  \n" > sysarg
		printf "void munge_wwwwwwww(const void *, void *);  \n" > sysarg
		printf "void munge_d(const void *, void *);  \n" > sysarg
		printf "void munge_dd(const void *, void *);  \n" > sysarg
		printf "void munge_ddd(const void *, void *);  \n" > sysarg
		printf "void munge_dddd(const void *, void *);  \n" > sysarg
		printf "void munge_ddddd(const void *, void *);  \n" > sysarg
		printf "void munge_dddddd(const void *, void *);  \n" > sysarg
		printf "void munge_ddddddd(const void *, void *);  \n" > sysarg
		printf "void munge_dddddddd(const void *, void *);  \n" > sysarg
		printf "void munge_wl(const void *, void *);  \n" > sysarg
		printf "void munge_wlw(const void *, void *);  \n" > sysarg
		printf "void munge_wwwl(const void *, void *);  \n" > sysarg
		printf "void munge_wwwwl(const void *, void *);  \n" > sysarg
		printf "void munge_wwwwwl(const void *, void *);  \n" > sysarg
		printf "void munge_wsw(const void *, void *);  \n" > sysarg
		printf "void munge_wws(const void *, void *);  \n" > sysarg
		printf "void munge_wwwsw(const void *, void *);  \n" > sysarg
		printf "#else \n" > sysarg
		printf "#define munge_w  NULL \n" > sysarg
		printf "#define munge_ww  NULL \n" > sysarg
		printf "#define munge_www  NULL \n" > sysarg
		printf "#define munge_wwww  NULL \n" > sysarg
		printf "#define munge_wwwww  NULL \n" > sysarg
		printf "#define munge_wwwwww  NULL \n" > sysarg
		printf "#define munge_wwwwwww  NULL \n" > sysarg
		printf "#define munge_wwwwwwww  NULL \n" > sysarg
		printf "#define munge_d  NULL \n" > sysarg
		printf "#define munge_dd  NULL \n" > sysarg
		printf "#define munge_ddd  NULL \n" > sysarg
		printf "#define munge_dddd  NULL \n" > sysarg
		printf "#define munge_ddddd  NULL \n" > sysarg
		printf "#define munge_dddddd  NULL \n" > sysarg
		printf "#define munge_ddddddd  NULL \n" > sysarg
		printf "#define munge_dddddddd  NULL \n" > sysarg
		printf "#define munge_wl  NULL \n" > sysarg
		printf "#define munge_wlw  NULL \n" > sysarg
		printf "#define munge_wwwl  NULL \n" > sysarg
		printf "#define munge_wwwwl  NULL \n" > sysarg
		printf "#define munge_wwwwwl  NULL \n" > sysarg
		printf "#define munge_wsw  NULL \n" > sysarg
		printf "#define munge_wws  NULL \n" > sysarg
		printf "#define munge_wwwsw  NULL \n" > sysarg
		printf "#endif // __ppc__\n" > sysarg
		printf "#endif /* !__MUNGE_ONCE */\n" > sysarg
		
		printf "\n" > sysarg

		printf "const char *%s[] = {\n", namesname > syscallnamestempfile
		next
	}
	NF == 0 || $1 ~ /^;/ {
		next
	}
	$1 ~ /^#[ 	]*include/ {
		print > sysinc
		next
	}
	$1 ~ /^#[ 	]*if/ {
		print > sysent
		print > sysarg
		print > syscallnamestempfile
		print > syshdrtempfile
		print > sysprotoend
		savesyscall = syscall
		next
	}
	$1 ~ /^#[ 	]*else/ {
		print > sysent
		print > sysarg
		print > syscallnamestempfile
		print > syshdrtempfile
		print > sysprotoend
		syscall = savesyscall
		next
	}
	$1 ~ /^#/ {
		print > sysent
		print > sysarg
		print > syscallnamestempfile
		print > syshdrtempfile
		print > sysprotoend
		next
	}
	syscall != $1 {
		printf "%s: line %d: syscall number out of sync at %d\n",
		    infile, NR, syscall
		printf "line is:\n"
		print
		exit 1
	}
	function align_comment(linesize, location, thefile) {
		printf(" ") > thefile
		while (linesize < location) {
			printf(" ") > thefile
			linesize++
		}
	}
	function parserr(was, wanted) {
		printf "%s: line %d: unexpected %s (expected %s)\n",
		    infile, NR, was, wanted
		exit 1
	}
	
	function parseline() {
		funcname = ""
		current_field = 5
		args_start = 0
		args_end = 0
		comments_start = 0
		comments_end = 0
		argc = 0
		argssize = "0"
		additional_comments = " "

		# find start and end of call name and arguments
		if ($current_field != "{")
			parserr($current_field, "{")
		args_start = current_field
		current_field++
		while (current_field <= NF) {
			if ($current_field == "}") {
				args_end = current_field
				break
			}
			current_field++
		}
		if (args_end == 0) {
			printf "%s: line %d: invalid call name and arguments\n",
		    	infile, NR
			exit 1
		}

		# find start and end of optional comments
		current_field++
		if (current_field < NF && $current_field == "{") {
			comments_start = current_field
			while (current_field <= NF) {
				if ($current_field == "}") {
					comments_end = current_field
					break
				}
				current_field++
			}
			if (comments_end == 0) {
				printf "%s: line %d: invalid comments \n",
					infile, NR
				exit 1
			}
		}

		if ($args_end != "}")
			parserr($args_end, "}")
		args_end--
		if ($args_end != ";")
			parserr($args_end, ";")
		args_end--
		if ($args_end != ")")
			parserr($args_end, ")")
		args_end--

		# extract additional comments
		if (comments_start != 0) {
			current_field = comments_start + 1
			while (current_field < comments_end) {
				additional_comments = additional_comments $current_field " "
				current_field++
			}
		}

		# get function return type
		current_field = args_start + 1
		returntype = $current_field

		# get function name and set up to get arguments
		current_field++
		funcname = $current_field
		argalias = funcname "_args"
		current_field++ # bump past function name

		if ($current_field != "(")
			parserr($current_field, "(")
		current_field++

		if (current_field == args_end) {
			if ($current_field != "void")
				parserr($current_field, "argument definition")
			return
		}

		# extract argument types and names
		while (current_field <= args_end) {
			argc++
			argtype[argc]=""
			ext_argtype[argc]=""
			oldf=""
			while (current_field < args_end && $(current_field + 1) != ",") {
				if (argtype[argc] != "" && oldf != "*") {
					argtype[argc] = argtype[argc] " ";
				}
				argtype[argc] = argtype[argc] $current_field;
				ext_argtype[argc] = argtype[argc];
				oldf = $current_field;
				current_field++
			}
			if (argtype[argc] == "")
				parserr($current_field, "argument definition")
			argname[argc] = $current_field;
			current_field += 2;			# skip name, and any comma
		}
		if (argc > 8) {
			printf "%s: line %d: too many arguments!\n", infile, NR
			exit 1
		}
		if (argc != 0)
			argssize = "AC(" argalias ")"
	}
	
	{
		add_sysent_entry = 1
		add_sysnames_entry = 1
		add_sysheader_entry = 1
		add_sysproto_entry = 1
		add_64bit_unsafe = 0
		add_64bit_fakesafe = 0
		add_cancel_enable = "0"

		if ($2 == "NONE") {
			add_cancel_enable = "_SYSCALL_CANCEL_NONE"
		}
		else if ($2 == "PRE") {
			add_cancel_enable = "_SYSCALL_CANCEL_PRE"
		}
		else if ($2 == "POST") {
			add_cancel_enable = "_SYSCALL_CANCEL_POST"
		} 
		else {
			printf "%s: line %d: unrecognized keyword %s\n", infile, NR, $2
			exit 1

		}

		if ($3 == "KERN") {
			my_funnel = "KERNEL_FUNNEL"
		}
		else if ($3 == "NONE") {
			my_funnel = "NO_FUNNEL"
		}
		else {
			printf "%s: line %d: unrecognized keyword %s\n", infile, NR, $3
			exit 1
		}
		
		if ($4 != "ALL" && $4 != "UALL") {
			files_keyword_OK = 0
			add_sysent_entry = 0
			add_sysnames_entry = 0
			add_sysheader_entry = 0
			add_sysproto_entry = 0
			
			if (match($4, "[T]") != 0) {
				add_sysent_entry = 1
				files_keyword_OK = 1
			}
			if (match($4, "[N]") != 0) {
				add_sysnames_entry = 1
				files_keyword_OK = 1
			}
			if (match($4, "[H]") != 0) {
				add_sysheader_entry = 1
				files_keyword_OK = 1
			}
			if (match($4, "[P]") != 0) {
				add_sysproto_entry = 1
				files_keyword_OK = 1
			}
			if (match($4, "[U]") != 0) {
				add_64bit_unsafe = 1
			}
			if (match($4, "[F]") != 0) {
				add_64bit_fakesafe = 1
			}
			
			if (files_keyword_OK == 0) {
				printf "%s: line %d: unrecognized keyword %s\n", infile, NR, $4
				exit 1
			}
		}
		else if ($4 == "UALL") {
			add_64bit_unsafe = 1;
		}
		
		
		parseline()
		
		# output function argument structures to sysproto.h and build the
		# name of the appropriate argument mungers
		munge32 = "NULL"
		munge64 = "NULL"
		if (funcname != "nosys" || (syscall == 0 && funcname == "nosys")) {
			if (argc != 0) {
				if (add_sysproto_entry == 1) {
					printf("struct %s {\n", argalias) > sysarg
				}
				munge32 = "munge_"
				munge64 = "munge_"
				for (i = 1; i <= argc; i++) {
					# Build name of argument munger.
					# We account for all sys call argument types here.
					# This is where you add any new types.  With LP64 support
					# each argument consumes 64-bits.  
					# see .../xnu/bsd/dev/ppc/munge.s for munge argument types.
					if (argtype[i] == "long") {
						if (add_64bit_unsafe == 0)
							ext_argtype[i] = "user_long_t";
						munge32 = munge32 "s"
						munge64 = munge64 "d"
					}
					else if (argtype[i] == "u_long") {
						if (add_64bit_unsafe == 0)
							ext_argtype[i] = "user_ulong_t";
						munge32 = munge32 "w"
						munge64 = munge64 "d"
					}
					else if (argtype[i] == "size_t") {
						if (add_64bit_unsafe == 0)
							ext_argtype[i] = "user_size_t";
						munge32 = munge32 "w"
						munge64 = munge64 "d"
					}
					else if (argtype[i] == "ssize_t") {
						if (add_64bit_unsafe == 0)
							ext_argtype[i] = "user_ssize_t";
						munge32 = munge32 "s"
						munge64 = munge64 "d"
					}
					else if (argtype[i] == "user_ssize_t" || argtype[i] == "user_long_t") {
						munge32 = munge32 "s"
						munge64 = munge64 "d"
					}
					else if (argtype[i] == "user_addr_t" || argtype[i] == "user_size_t" ||
						argtype[i] == "user_ulong_t") {
						munge32 = munge32 "w"
						munge64 = munge64 "d"
					}
					else if (argtype[i] == "caddr_t" || argtype[i] == "semun_t" ||
  						match(argtype[i], "[\*]") != 0) {
						if (add_64bit_unsafe == 0)
							ext_argtype[i] = "user_addr_t";
						munge32 = munge32 "w"
						munge64 = munge64 "d"
					}
					else if (argtype[i] == "int" || argtype[i] == "u_int" ||
							 argtype[i] == "uid_t" || argtype[i] == "pid_t" ||
							 argtype[i] == "id_t" || argtype[i] == "idtype_t" ||
							 argtype[i] == "socklen_t" || argtype[i] == "uint32_t" || argtype[i] == "int32_t" ||
							 argtype[i] == "sigset_t" || argtype[i] == "gid_t" ||
							 argtype[i] == "mode_t" || argtype[i] == "key_t" || argtype[i] == "time_t") {
						munge32 = munge32 "w"
						munge64 = munge64 "d"
					}
					else if (argtype[i] == "off_t" || argtype[i] == "int64_t" || argtype[i] == "uint64_t") {
						munge32 = munge32 "l"
						munge64 = munge64 "d"
					}
					else {
						printf "%s: line %d: invalid type \"%s\" \n", 
							infile, NR, argtype[i]
						printf "You need to add \"%s\" into the type checking code. \n", 
							 argtype[i]
						exit 1
					}
					if (add_sysproto_entry == 1) {
						printf("\tchar %s_l_[PADL_(%s)]; " \
							"%s %s; char %s_r_[PADR_(%s)];\n",
							argname[i], ext_argtype[i],
							ext_argtype[i], argname[i],
							argname[i], ext_argtype[i]) > sysarg
					}
				}
				if (add_sysproto_entry == 1) {
					printf("};\n") > sysarg
				}
			}
			else if (add_sysproto_entry == 1) { 
				printf("struct %s {\n\tregister_t dummy;\n};\n", argalias) > sysarg
			}
		}
		
		# output to init_sysent.c
		tempname = funcname
		if (add_sysent_entry == 0) {
			argssize = "0"
			munge32 = "NULL"
			munge64 = "NULL"
			munge_ret = "_SYSCALL_RET_NONE"
			tempname = "nosys"
		}
		else {
			# figure out which return value type to munge
			if (returntype == "user_addr_t") {
				munge_ret = "_SYSCALL_RET_ADDR_T"
			}
			else if (returntype == "user_ssize_t") {
				munge_ret = "_SYSCALL_RET_SSIZE_T"
			}
			else if (returntype == "user_size_t") {
				munge_ret = "_SYSCALL_RET_SIZE_T"
			}
			else if (returntype == "int") {
				munge_ret = "_SYSCALL_RET_INT_T"
			}
			else if (returntype == "u_int") {
				munge_ret = "_SYSCALL_RET_UINT_T"
			}
			else if (returntype == "off_t") {
				munge_ret = "_SYSCALL_RET_OFF_T"
			}
			else if (returntype == "void") {
				munge_ret = "_SYSCALL_RET_NONE"
			}
			else {
				printf "%s: line %d: invalid return type \"%s\" \n", 
					infile, NR, returntype
				printf "You need to add \"%s\" into the return type checking code. \n", 
					 returntype
				exit 1
			}
		}

		if (add_64bit_unsafe == 1  && add_64bit_fakesafe == 0)
			my_funnel = my_funnel "|UNSAFE_64BIT";

		printf("\t{%s, %s, %s, \(sy_call_t *\)%s, %s, %s, %s},", 
				argssize, add_cancel_enable, my_funnel, tempname, munge32, munge64, munge_ret) > sysent
		linesize = length(argssize) + length(add_cancel_enable) + length(my_funnel) + length(tempname) + \
				length(munge32) + length(munge64) + length(munge_ret) + 28
		align_comment(linesize, 88, sysent)
		printf("/* %d = %s%s*/\n", syscall, funcname, additional_comments) > sysent
		
		# output to syscalls.c
		if (add_sysnames_entry == 1) {
			tempname = funcname
			if (funcname == "nosys") {
				if (syscall == 0)
					tempname = "syscall"
				else
					tempname = "#" syscall
			}
			printf("\t\"%s\", ", tempname) > syscallnamestempfile
			linesize = length(tempname) + 8
			align_comment(linesize, 25, syscallnamestempfile)
			if (substr(tempname,1,1) == "#") {
				printf("/* %d =%s*/\n", syscall, additional_comments) > syscallnamestempfile
			}
			else {
				printf("/* %d = %s%s*/\n", syscall, tempname, additional_comments) > syscallnamestempfile
			}
		}

		# output to syscalls.h
		if (add_sysheader_entry == 1) {
			tempname = funcname
			if (syscall == 0) {
				tempname = "syscall"
			}
			if (tempname != "nosys") {
				printf("#define\t%s%s", syscallprefix, tempname) > syshdrtempfile
				linesize = length(syscallprefix) + length(tempname) + 12
				align_comment(linesize, 30, syshdrtempfile)
				printf("%d\n", syscall) > syshdrtempfile
				# special case for gettimeofday on ppc - cctools project uses old name
				if (tempname == "ppc_gettimeofday") {
					printf("#define\t%s%s", syscallprefix, "gettimeofday") > syshdrtempfile
					linesize = length(syscallprefix) + length(tempname) + 12
					align_comment(linesize, 30, syshdrtempfile)
					printf("%d\n", syscall) > syshdrtempfile
				}
			}
			else {
				printf("\t\t\t/* %d %s*/\n", syscall, additional_comments) > syshdrtempfile
			}
		}
		
		# output function prototypes to sysproto.h
		if (add_sysproto_entry == 1) {
			if (funcname =="exit") {
				printf("void %s(struct proc *, struct %s *, int *);\n", 
						funcname, argalias) > sysprotoend
			}
			else if (funcname != "nosys" || (syscall == 0 && funcname == "nosys")) {
				printf("int %s(struct proc *, struct %s *, %s *);\n", 
						funcname, argalias, returntype) > sysprotoend
			}
		}
		
		syscall++
		next
	}

	END {
		printf "#ifdef __ppc__\n" > sysinc
		printf "#define AC(name) (sizeof(struct name) / sizeof(uint64_t))\n" > sysinc
		printf "#else\n" > sysinc
		printf "#define AC(name) (sizeof(struct name) / sizeof(register_t))\n" > sysinc
		printf "#endif\n" > sysinc
		printf "\n" > sysinc

		printf("\n__END_DECLS\n") > sysprotoend
		printf("#undef PAD_\n") > sysprotoend
		printf("#undef PADL_\n") > sysprotoend
		printf("#undef PADR_\n") > sysprotoend
		printf "\n#endif /* __APPLE_API_PRIVATE */\n" > sysprotoend
		printf "#endif /* KERNEL */\n" > sysprotoend
		printf("\n#endif /* !%s */\n", sysproto_h) > sysprotoend

		printf("};\n") > sysent
		printf("int	nsysent = sizeof(sysent) / sizeof(sysent[0]);\n") > sysent

		printf("};\n") > syscallnamestempfile
		printf("#define\t%sMAXSYSCALL\t%d\n", syscallprefix, syscall) \
		    > syshdrtempfile
		printf("\n#endif /* __APPLE_API_PRIVATE */\n") > syshdrtempfile
		printf("#endif /* !%s */\n", syscall_h) > syshdrtempfile
	} '

cat $syslegal $sysinc $sysent > $syscalltablefile
cat $syslegal $sysarg $sysprotoend > $sysprotofile
cat $syslegal $syscallnamestempfile > $syscallnamesfile
cat $syslegal $syshdrtempfile > $syshdrfile
