/* Configuration file for Linux Utopia targets.
   Copyright (C) 2008-2022 Free Software Foundation, Inc.
   Contributed by Diego Magdaleno (diegomagdaleno@protonmail.com)

   This file is part of GCC.

   GCC is free software; you can redistribute it and/or modify it
   under the terms of the GNU General Public License as published
   by the Free Software Foundation; either version 3, or (at your
   option) any later version.

   GCC is distributed in the hope that it will be useful, but WITHOUT
   ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
   or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public
   License for more details.

   You should have received a copy of the GNU General Public License
   along with GCC; see the file COPYING3.  If not see
   <http://www.gnu.org/licenses/>.  */

#define UTOPIA_TARGET_OS_CPP_BUILTINS()         \
    do {            \
        if (TARGET_UTOPIA)              \
        builtin_define("__UTOPIA__");               \
    } while (0)

#define UTOPIA_TARGET_D_OS_VERSIONS()             \
    do {        \
        if (TARGET_UTOPIA)       \
	  builtin_version ("Utopia");				\
    } while (0)

#if UTOPIA_DEFAULT
# define NOUTOPIA "mno-utopia"
#else
# define NOUTOPIA "!mutopia"
#endif

#define LINUX_OR_UTOPIA_CC(LINUX_SPEC, UTOPIA_SPEC) \
    "%{" NOUTOPIA "|tno-utopia-cc:"  LINUX_SPEC ";;" UTOPIA_SPEC "}"

#define LINUX_OR_UTOPIA_LD(LINUX_SPEC, UTOPIA_SPEC) \
    "%{" NOUTOPIA "|tno-utopia-ld:"  LINUX_SPEC ";;" UTOPIA_SPEC "}"

/*
 *  
 * When on Utopia we define -musl as it is the default libc in Utopia (or at least what it is based on)
 * whe also tell the linker to link libSystem, which is the name we gave to the libc in Utopia.
 * 
 * For the start and end files, we let the compiler perform its operations as expected, Utopia doesn't need
 * any changes on this part.
 * 
*/

#define UTOPIA_CC1_SPEC         \
      "%{!mglibc:%{!muclibc:%{!mmusl: -mmusl}}} "
    
#define UTOPIA_LIB_SPEC         \
       "%{!static: -lSystem}"
