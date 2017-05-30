/*
 * Copyright (C) 2001-2012 Philippe Gerum <rpm@xenomai.org>.
 * Copyright (C) 2004,2005 Gilles Chanteperdrix <gilles.chanteperdrix@xenomai.org>.
 *
 * Xenomai is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published
 * by the Free Software Foundation; either version 2 of the License,
 * or (at your option) any later version.
 *
 * Xenomai is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Xenomai; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 * 02111-1307, USA.
 */
#include <linux/module.h>
#include <steely/lock.h>

DEFINE_XNLOCK(nklock);
#if defined(CONFIG_SMP) || STEELY_DEBUG(LOCKING)
EXPORT_SYMBOL_GPL(nklock);

#ifdef CONFIG_STEELY_ARCH_OUTOFLINE_XNLOCK
int ___xnlock_get(struct xnlock *lock /*, */ XNLOCK_DBG_CONTEXT_ARGS)
{
	return ____xnlock_get(lock /* , */ XNLOCK_DBG_PASS_CONTEXT);
}
EXPORT_SYMBOL_GPL(___xnlock_get);

void ___xnlock_put(struct xnlock *lock /*, */ XNLOCK_DBG_CONTEXT_ARGS)
{
	____xnlock_put(lock /* , */ XNLOCK_DBG_PASS_CONTEXT);
}
EXPORT_SYMBOL_GPL(___xnlock_put);
#endif /* out of line xnlock */
#endif /* CONFIG_SMP || STEELY_DEBUG(LOCKING) */

#if STEELY_DEBUG(LOCKING)
DEFINE_PER_CPU(struct xnlockinfo, xnlock_stats);
EXPORT_PER_CPU_SYMBOL_GPL(xnlock_stats);
#endif
