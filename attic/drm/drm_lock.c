/* BEGIN CSTYLED */

/**
 * \file drm_lock.c
 * IOCTLs for locking
 *
 * \author Rickard E. (Rik) Faith <faith@valinux.com>
 * \author Gareth Hughes <gareth@valinux.com>
 */

/*
 * Created: Tue Feb  2 08:37:54 1999 by faith@valinux.com
 *
 * Copyright 1999 Precision Insight, Inc., Cedar Park, Texas.
 * Copyright 2000 VA Linux Systems, Inc., Sunnyvale, California.
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * VA LINUX SYSTEMS AND/OR ITS SUPPLIERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

/*
 * Copyright 2010 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 */

#include "drmP.h"

/**
 * Lock ioctl.
 *
 * \param inode device inode.
 * \param file_priv DRM file private.
 * \param cmd command.
 * \param arg user argument, pointing to a drm_lock structure.
 * \return zero on success or negative number on failure.
 *
 * Add the current task to the lock wait queue, and attempt to take to lock.
 */
int drm_lock(DRM_IOCTL_ARGS)
{
	struct drm_lock *lock = data;
	struct drm_master *master = file_priv->master;
	int ret = 0;

	++file_priv->lock_count;

	if (lock->context == DRM_KERNEL_CONTEXT) {
		DRM_ERROR("Process %d using kernel context %d\n",
		    DRM_CURRENTPID, lock->context);
		return -EINVAL;
	}

	DRM_DEBUG("%d (pid %d) requests lock (0x%08x), flags = 0x%08x\n",
		  lock->context, DRM_CURRENTPID,
		  master->lock.hw_lock->lock, lock->flags);

	if (drm_core_check_feature(dev, DRIVER_DMA_QUEUE))
		if (lock->context < 0)
			return -EINVAL;

	mutex_enter(&master->lock.lock_mutex);
	master->lock.user_waiters++;
	for (;;) {
		if (drm_lock_take(&master->lock, lock->context)) {
			master->lock.file_priv = file_priv;
			master->lock.lock_time = ddi_get_lbolt();
			atomic_inc(&dev->counts[_DRM_STAT_LOCKS]);
			break;	/* Got lock */
		}

		ret = cv_wait_sig(&master->lock.lock_cv,
		    &master->lock.lock_mutex);
		if (ret == 0) {
			ret = -EINTR;
			break;
		}
	}
	master->lock.user_waiters--;
	mutex_exit(&master->lock.lock_mutex);

	DRM_DEBUG("%d %s\n", lock->context,
		  ret ? "interrupted" : "has lock");
	if (ret) return ret;


	if (dev->driver->dma_ready && (lock->flags & _DRM_LOCK_READY))
		dev->driver->dma_ready(dev);

	if (dev->driver->dma_quiescent && (lock->flags & _DRM_LOCK_QUIESCENT))
	{
		if (dev->driver->dma_quiescent(dev)) {
			DRM_DEBUG("%d waiting for DMA quiescent\n",
				  lock->context);
			return -EBUSY;
		}
	}

	if (dev->driver->kernel_context_switch &&
	    dev->last_context != lock->context) {
		dev->driver->kernel_context_switch(dev, dev->last_context,
						   lock->context);
	}

	return 0;
}

/**
 * Unlock ioctl.
 *
 * \param inode device inode.
 * \param file_priv DRM file private.
 * \param cmd command.
 * \param arg user argument, pointing to a drm_lock structure.
 * \return zero on success or negative number on failure.
 *
 * Transfer and free the lock.
 */
int drm_unlock(DRM_IOCTL_ARGS)
{
	struct drm_lock *lock = data;
	struct drm_master *master = file_priv->master;

	if (lock->context == DRM_KERNEL_CONTEXT) {
		DRM_ERROR("Process %d using kernel context %d\n",
		    DRM_CURRENTPID, lock->context);
		return -EINVAL;
	}

	atomic_inc(&dev->counts[_DRM_STAT_UNLOCKS]);

	/* kernel_context_switch isn't used by any of the x86 drm
	 * modules but is required by the Sparc driver.
	 */
	if (dev->driver->kernel_context_switch_unlock)
		dev->driver->kernel_context_switch_unlock(dev);
	else {
		if (drm_lock_free(&master->lock, lock->context)) {
			/* FIXME: Should really bail out here. */
		}
	}

	return 0;
}

/**
 * Take the heavyweight lock.
 *
 * \param lock lock pointer.
 * \param context locking context.
 * \return one if the lock is held, or zero otherwise.
 *
 * Attempt to mark the lock as held by the given context, via the \p cmpxchg instruction.
 */
int drm_lock_take(struct drm_lock_data *lock_data,
		  unsigned int context)
{
	unsigned int old, new;
	volatile unsigned int *lock = &lock_data->hw_lock->lock;

	do {
		old = *lock;
		if (old & _DRM_LOCK_HELD)
			new = old | _DRM_LOCK_CONT;
		else {
			new = context | _DRM_LOCK_HELD |
				((lock_data->user_waiters + lock_data->kernel_waiters > 1) ?
				 _DRM_LOCK_CONT : 0);
		}
	} while (!atomic_cmpset_int(lock, old, new));

	if (_DRM_LOCKING_CONTEXT(old) == context) {
		if (old & _DRM_LOCK_HELD) {
			if (context != DRM_KERNEL_CONTEXT) {
				DRM_ERROR("%d holds heavyweight lock\n",
					  context);
			}
			return 0;
		}
	}

	if ((_DRM_LOCKING_CONTEXT(new)) == context && (new & _DRM_LOCK_HELD)) {
		/* Have lock */
		return 1;
	}
	return 0;
}

/**
 * This takes a lock forcibly and hands it to context.	Should ONLY be used
 * inside *_unlock to give lock to kernel before calling *_dma_schedule.
 *
 * \param dev DRM device.
 * \param lock lock pointer.
 * \param context locking context.
 * \return always one.
 *
 * Resets the lock file pointer.
 * Marks the lock as held by the given context, via the \p cmpxchg instruction.
 */
static int drm_lock_transfer(struct drm_lock_data *lock_data,
			     unsigned int context)
{
	unsigned int old, new;
	volatile unsigned int *lock = &lock_data->hw_lock->lock;

	lock_data->file_priv = NULL;
	do {
		old = *lock;
		new = context | _DRM_LOCK_HELD;
	} while (!atomic_cmpset_int(lock, old, new));
	return 1;
}

/**
 * Free lock.
 *
 * \param dev DRM device.
 * \param lock lock.
 * \param context context.
 *
 * Resets the lock file pointer.
 * Marks the lock as not held, via the \p cmpxchg instruction. Wakes any task
 * waiting on the lock queue.
 */
int drm_lock_free(struct drm_lock_data *lock_data, unsigned int context)
{
	unsigned int old, new;
	volatile unsigned int *lock = &lock_data->hw_lock->lock;

	mutex_enter(&lock_data->lock_mutex);
	if (lock_data->kernel_waiters != 0) {
		drm_lock_transfer(lock_data, 0);
		lock_data->idle_has_lock = 1;
		mutex_exit(&lock_data->lock_mutex);
		return 1;
	}

	do {
		old = *lock;
		new = _DRM_LOCKING_CONTEXT(old);
	} while (!atomic_cmpset_int(lock, old, new));

	if (_DRM_LOCK_IS_HELD(old) && _DRM_LOCKING_CONTEXT(old) != context) {
		DRM_ERROR("%d freed heavyweight lock held by %d\n",
			  context, _DRM_LOCKING_CONTEXT(old));
		mutex_exit(&lock_data->lock_mutex);
		return 1;
	}
	cv_broadcast(&lock_data->lock_cv);
	mutex_exit(&lock_data->lock_mutex);
	return 0;
}

/**
 * This function returns immediately and takes the hw lock
 * with the kernel context if it is free, otherwise it gets the highest priority when and if
 * it is eventually released.
 *
 * This guarantees that the kernel will _eventually_ have the lock _unless_ it is held
 * by a blocked process. (In the latter case an explicit wait for the hardware lock would cause
 * a deadlock, which is why the "idlelock" was invented).
 *
 * This should be sufficient to wait for GPU idle without
 * having to worry about starvation.
 */

void drm_idlelock_take(struct drm_lock_data *lock_data)
{
	int ret = 0;

	mutex_enter(&lock_data->lock_mutex);
	lock_data->kernel_waiters++;
	if (!lock_data->idle_has_lock) {

		ret = drm_lock_take(lock_data, DRM_KERNEL_CONTEXT);

		if (ret == 1)
			lock_data->idle_has_lock = 1;
	}
	mutex_exit(&(lock_data->lock_mutex));
}

void drm_idlelock_release(struct drm_lock_data *lock_data)
{
	unsigned int old;
	volatile unsigned int *lock = &lock_data->hw_lock->lock;

	mutex_enter(&lock_data->lock_mutex);
	if (--lock_data->kernel_waiters == 0) {
		if (lock_data->idle_has_lock) {
			do {
				old = *lock;
			} while (!atomic_cmpset_int(lock, old, DRM_KERNEL_CONTEXT));
			cv_broadcast(&lock_data->lock_cv);
			lock_data->idle_has_lock = 0;
		}
	}
	mutex_exit(&lock_data->lock_mutex);
}


int drm_i_have_hw_lock(struct drm_device *dev, struct drm_file *file_priv)
{
	struct drm_master *master = file_priv->master;
	return (file_priv->lock_count && master->lock.hw_lock &&
		_DRM_LOCK_IS_HELD(master->lock.hw_lock->lock) &&
		master->lock.file_priv == file_priv);
}