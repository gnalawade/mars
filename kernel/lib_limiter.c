/*
 * MARS Long Distance Replication Software
 *
 * This file is part of MARS project: http://schoebel.github.io/mars/
 *
 * Copyright (C) 2010-2014 Thomas Schoebel-Theuer
 * Copyright (C) 2011-2014 1&1 Internet AG
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */


#include "lib_limiter.h"

#include <linux/kernel.h>
#include <linux/module.h>

#define LIMITER_TIME_RESOLUTION NSEC_PER_SEC

int mars_limit(struct mars_limiter *lim, int amount)
{
	int delay = 0;
	long long now;

	if (unlikely(amount < 0))
		amount = 0;

	now = cpu_clock(raw_smp_processor_id());

	/* Compute the maximum delay along the path
	 * down to the root of the hierarchy tree.
	 */
	while (lim != NULL) {
		long long window = now - lim->lim_stamp;
		/* Sometimes, raw CPU clocks may do weired things...
		 * Smaller windows in the denominator than 1s could fake unrealistic rates.
		 */
		if (unlikely(lim->lim_min_window <= 0))
			lim->lim_min_window = 1000;
		if (unlikely(lim->lim_max_window <= lim->lim_min_window))
			lim->lim_max_window = lim->lim_min_window + 8000;
		if (unlikely(window < (long long)lim->lim_min_window * (LIMITER_TIME_RESOLUTION / 1000)))
			window = (long long)lim->lim_min_window * (LIMITER_TIME_RESOLUTION / 1000);

		/* Update total statistics.
		 * They will intentionally wrap around.
		 * Userspace must take care of that.
		 */
		if (likely(amount > 0)) {
			lim->lim_total_amount += amount;
			lim->lim_total_ops++;
		}

		/* Only use incremental accumulation at repeated calls, but
		 * never after longer pauses.
		 */
		if (likely(lim->lim_stamp &&
			   window < (long long)lim->lim_max_window * (LIMITER_TIME_RESOLUTION / 1000))) {
			long long rate_raw;
			int rate;
			int max_rate;
			
			/* Races are possible, but taken into account.
			 * There is no real harm from rarely lost updates.
			 */
			if (likely(amount > 0)) {
				lim->lim_amount_accu += amount;
				lim->lim_amount_cumul += amount;
				lim->lim_ops_accu++;
				lim->lim_ops_cumul++;
			}

			/* compute amount values */
			rate_raw = lim->lim_amount_accu * LIMITER_TIME_RESOLUTION / window;
			rate = rate_raw;
			if (unlikely(rate_raw > INT_MAX)) {
				rate = INT_MAX;
			}
			lim->lim_amount_rate = rate;
			
			/* amount limit exceeded? */
			max_rate = lim->lim_max_amount_rate;
			if (max_rate > 0 && rate > max_rate) {
				int this_delay = (window * rate / max_rate - window) / (LIMITER_TIME_RESOLUTION / 1000);
				// compute maximum
				if (this_delay > delay && this_delay > 0)
					delay = this_delay;
			}

			/* compute ops values */
			rate_raw = lim->lim_ops_accu * LIMITER_TIME_RESOLUTION / window;
			rate = rate_raw;
			if (unlikely(rate_raw > INT_MAX)) {
				rate = INT_MAX;
			}
			lim->lim_ops_rate = rate;
			
			/* ops limit exceeded? */
			max_rate = lim->lim_max_ops_rate;
			if (max_rate > 0 && rate > max_rate) {
				int this_delay = (window * rate / max_rate - window) / (LIMITER_TIME_RESOLUTION / 1000);
				// compute maximum
				if (this_delay > delay && this_delay > 0)
					delay = this_delay;
			}

			/* Try to keep the next window below min_window
			 */
			window -= lim->lim_min_window * (LIMITER_TIME_RESOLUTION / 1000);
			if (window > 0) {
				long long used_up = (long long)lim->lim_amount_rate * window / LIMITER_TIME_RESOLUTION;
				if (used_up > 0) {
					lim->lim_stamp += window;
					lim->lim_amount_accu -= used_up;
					if (unlikely(lim->lim_amount_accu < 0))
						lim->lim_amount_accu = 0;
				}
				used_up = (long long)lim->lim_ops_rate * window / LIMITER_TIME_RESOLUTION;
				if (used_up > 0) {
					lim->lim_stamp += window;
					lim->lim_ops_accu -= used_up;
					if (unlikely(lim->lim_ops_accu < 0))
						lim->lim_ops_accu = 0;
				}
			}
		} else { // reset, start over with new measurement cycle
			if (unlikely(amount < 0))
				amount = 0;
			lim->lim_ops_accu = 1;
			lim->lim_amount_accu = amount;
			lim->lim_stamp = now - lim->lim_min_window * (LIMITER_TIME_RESOLUTION / 1000);
			lim->lim_ops_rate = 0;
			lim->lim_amount_rate = 0;
		}
		lim = lim->lim_father;
	}
	return delay;
}
EXPORT_SYMBOL_GPL(mars_limit);

void mars_limit_sleep(struct mars_limiter *lim, int amount)
{
	int sleep = mars_limit(lim, amount);
	if (sleep > 0) {
		if (unlikely(lim->lim_max_delay <= 0))
			lim->lim_max_delay = 1000;
		if (sleep > lim->lim_max_delay)
			sleep = lim->lim_max_delay;
		brick_msleep(sleep);
	}
}
EXPORT_SYMBOL_GPL(mars_limit_sleep);

void mars_limit_reset(struct mars_limiter *lim)
{
	if (!lim)
		return;
	lim->lim_stamp = 0;
	mars_limit(lim, 0);
}
