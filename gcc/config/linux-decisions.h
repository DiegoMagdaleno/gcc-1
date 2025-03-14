/* Decide what linker we should use or cc when on Linux platforms
   with different operating systems.
   Copyright (C) 2008-2021 Free Software Foundation, Inc.
   Contributed by Diego Magdaleno (diegomagdaleno@protonmail.com)
  
   This file is part of GCC
 
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
   <http://www.gnu.org/licenses/>.   */
#ifndef LINUX_DECISIONS_H
#define LINUX_DECISIONS_H

#if ANDROID_DEFAULT
# define NOANDROID "mno-android"
#else
# define NOANDROID "!mandroid"
#endif

#if UTOPIA_DEFAULT
# define NOUTOPIA "mno-utopia"
#else
# define NOUTOPIA "!mutopia"
#endif

/* Reference for this syntax: https://gcc.gnu.org/onlinedocs/gcc/Spec-Files.html 
   I am new to this I dont know if this is correct but the logic behind this is
   if tno-android-cc is defined or NOANDROID then we fall back to the Utopia spec, then
   we check if the t-noutopia-cc is defined or NOUTOPIA is defined, in that case we fall back 
   to the LINUX_SPEC finally if none of this matched (aka both no utopia and no android are undefined)
   then we can make a fair guess we might be running on android.

   The logic above is probably extreamly wrong 
*/

#define LINUX_CHOOSE_SPEC_CC(LINUX_SPEC, ANDROID_SPEC, UTOPIA_SPEC) \
   "%{" NOANDROID "|tno-android-cc:" UTOPIA_SPEC ";:%{" NOUTOPIA "|tno-utopia-cc:" LINUX_SPEC ";:" ANDROID_SPEC "}}"

#define LINUX_CHOOSE_SPEC_LD(LINUX_SPEC, ANDROID_SPEC, UTOPIA_SPEC) \
   "%{" NOANDROID "|tno-android-ld:" UTOPIA_SPEC ";:%{" NOUTOPIA "|tno-utopia-ld:" LINUX_SPEC ";:" ANDROID_SPEC "}}"

#endif