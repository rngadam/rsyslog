/* This header supplies atomic operations. So far, we rely on GCC's
 * atomic builtins. During configure, we check if atomic operatons are
 * available. If they are not, I am making the necessary provisioning to live without them if
 * they are not available. Please note that you should only use the macros
 * here if you think you can actually live WITHOUT an explicit atomic operation,
 * because in the non-presence of them, we simply do it without atomicitiy.
 * Which, for word-aligned data types, usually (but only usually!) should work.
 *
 * We are using the functions described in
 * http:/gcc.gnu.org/onlinedocs/gcc/Atomic-Builtins.html
 *
 * THESE MACROS MUST ONLY BE USED WITH WORD-SIZED DATA TYPES!
 *
 * Copyright 2008 Rainer Gerhards and Adiscon GmbH.
 *
 * This file is part of the rsyslog runtime library.
 *
 * The rsyslog runtime library is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * The rsyslog runtime library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with the rsyslog runtime library.  If not, see <http://www.gnu.org/licenses/>.
 *
 * A copy of the GPL can be found in the file "COPYING" in this distribution.
 * A copy of the LGPL can be found in the file "COPYING.LESSER" in this distribution.
 */
#include "config.h" /* autotools! */

#ifndef INCLUDED_ATOMIC_H
#define INCLUDED_ATOMIC_H

/* for this release, we disable atomic calls because there seem to be some
 * portability problems and we can not fix that without destabilizing the build.
 * They simply came in too late. -- rgerhards, 2008-04-02
 */
#ifdef HAVE_ATOMIC_BUILTINS
#	define ATOMIC_INC(data) ((void) __sync_fetch_and_add(&(data), 1))
#	define ATOMIC_DEC_AND_FETCH(data) __sync_sub_and_fetch(&(data), 1)
#	define ATOMIC_FETCH_32BIT(data) ((unsigned) __sync_fetch_and_and(&(data), 0xffffffff))
#	define ATOMIC_STORE_1_TO_32BIT(data) __sync_lock_test_and_set(&(data), 1)
#else
#	warning "atomic builtins not available, using nul operations"
#	define ATOMIC_INC(data) (++(data))
#	define ATOMIC_DEC_AND_FETCH(data) (--(data))
#	define ATOMIC_FETCH_32BIT(data) (data)
#	define ATOMIC_STORE_1_TO_32BIT(data) (data) = 1
#endif

#endif /* #ifndef INCLUDED_ATOMIC_H */