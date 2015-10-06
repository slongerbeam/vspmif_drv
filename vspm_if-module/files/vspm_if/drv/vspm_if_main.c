/*************************************************************************/ /*
 VSPM

 Copyright (C) 2015 Renesas Electronics Corporation

 License        Dual MIT/GPLv2

 The contents of this file are subject to the MIT license as set out below.

 Permission is hereby granted, free of charge, to any person obtaining a copy
 of this software and associated documentation files (the "Software"), to deal
 in the Software without restriction, including without limitation the rights
 to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 copies of the Software, and to permit persons to whom the Software is
 furnished to do so, subject to the following conditions:

 The above copyright notice and this permission notice shall be included in
 all copies or substantial portions of the Software.

 Alternatively, the contents of this file may be used under the terms of
 the GNU General Public License Version 2 ("GPL") in which case the provisions
 of GPL are applicable instead of those above.

 If you wish to allow use of your version of this file only under the terms of
 GPL, and not to allow others to use your version of this file under the terms
 of the MIT license, indicate your decision by deleting the provisions above
 and replace them with the notice and other provisions required by GPL as set
 out in the file called "GPL-COPYING" included in this distribution. If you do
 not delete the provisions above, a recipient may use your version of this file
 under the terms of either the MIT license or GPL.

 This License is also included in this distribution in the file called
 "MIT-COPYING".

 EXCEPT AS OTHERWISE STATED IN A NEGOTIATED AGREEMENT: (A) THE SOFTWARE IS
 PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING
 BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR
 PURPOSE AND NONINFRINGEMENT; AND (B) IN NO EVENT SHALL THE AUTHORS OR
 COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.


 GPLv2:
 If you wish to use this file under the terms of GPL, following terms are
 effective.

 This program is free software; you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation; version 2 of the License.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with this program; if not, write to the Free Software
 Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
*/ /*************************************************************************/

#include <asm/uaccess.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/miscdevice.h>
#include <linux/slab.h>
#include <linux/dma-mapping.h>
#include <linux/fs.h>
#include <linux/ioctl.h>

#include "vspm_public.h"
#include "vspm_if.h"
#include "vspm_if_local.h"

struct device *dev;

static int open(struct inode *inode, struct file *file)
{
	struct vspm_if_private_t *priv;

	/* allocate memory */
	priv = kzalloc(sizeof(struct vspm_if_private_t), GFP_KERNEL);
	if (priv == NULL)
		return -ENOMEM;

	/* init */
	spin_lock_init(&priv->lock);
	init_completion(&priv->wait_interrupt);
	init_completion(&priv->wait_thread);
	INIT_LIST_HEAD(&priv->cb_data.list);

	file->private_data = priv;
	return 0;
}

static int close(struct inode *inode, struct file *file)
{
	struct vspm_if_private_t *priv =
		(struct vspm_if_private_t *)file->private_data;

	long ercd;

	if (priv != NULL) {
		if (priv->handle != 0) {
			ercd = vspm_quit_driver(priv->handle);
			if (ercd != R_VSPM_OK) {
				EPRINT("failed to vspm_quit_driver %d\n",
					(int)ercd);
				/* forced release memory */
				kfree(priv);
				return -EFAULT;
			}
			priv->handle = 0;
		}

		/* release memory */
		kfree(priv);
	}

	file->private_data = NULL;
	return 0;
}

static long vspm_ioctl_init(
	struct vspm_if_private_t *priv, unsigned int cmd, unsigned long arg)
{
	struct vspm_init_t init_par;
	struct vspm_init_fdp_t init_fdp_par;

	unsigned long handle;
	long ercd;

	/* copy initialize parameter */
	if (copy_from_user(
			&init_par, (void __user *)arg, _IOC_SIZE(cmd))) {
		EPRINT("INIT: failed to copy from user!!\n");
		return -EFAULT;
	}

	switch (init_par.type) {
	case VSPM_TYPE_VSP_AUTO:
		/* initialize parameter is not used */
		init_par.par.vsp = NULL;
		break;
	case VSPM_TYPE_FDP_AUTO:
		/* copy initialize parameter of FDP */
		if (init_par.par.fdp != NULL) {
			if (copy_from_user(
					&init_fdp_par,
					(void __user *)init_par.par.fdp,
					sizeof(struct vspm_init_fdp_t))) {
				EPRINT("INIT: failed to copy FDP param\n");
				return -EFAULT;
			}
			init_par.par.fdp = &init_fdp_par;
		}
		break;
	default:
		break;
	}

	/* initialize VSP manager */
	ercd = vspm_init_driver(&handle, &init_par);
	switch (ercd) {
	case R_VSPM_OK:
		break;
	case R_VSPM_PARAERR:
		return -EINVAL;
		break;
	case R_VSPM_ALREADY_USED:
		return -EBUSY;
		break;
	default:
		return -EFAULT;
		break;
	}

	priv->handle = handle;
	return 0;
}

static long vspm_ioctl_quit(struct vspm_if_private_t *priv)
{
	long ercd;

	/* finalize VSP manager */
	ercd = vspm_quit_driver(priv->handle);
	if (ercd != R_VSPM_OK)
		return -EFAULT;

	priv->handle = 0;
	return 0;
}

static void vspm_cb_func(
	unsigned long job_id, long result, unsigned long user_data)
{
	struct vspm_if_entry_data_t *entry_data =
		(struct vspm_if_entry_data_t *)user_data;

	struct vspm_if_private_t *priv;
	struct vspm_if_cb_data_t *cb_data;
	unsigned long lock_flag;

	if (entry_data == NULL)
		return;

	priv = entry_data->priv;

	/* allocate callback data */
	cb_data = kzalloc(sizeof(struct vspm_if_cb_data_t), GFP_ATOMIC);
	if (cb_data == NULL) {
		EPRINT("CB: failed to allocate memory\n");
		goto exit;
	}

	/* make response data */
	cb_data->rsp.ercd = 0;
	cb_data->rsp.cb_func = entry_data->entry.req.cb_func;
	cb_data->rsp.job_id = job_id;
	cb_data->rsp.result = result;
	cb_data->rsp.user_data = entry_data->entry.req.user_data;

	if (entry_data->job.type == VSPM_TYPE_VSP_AUTO) {
		/* set callback response of vsp */
		set_cb_rsp_vsp(cb_data, entry_data);
	}

	/* addition list */
	spin_lock_irqsave(&priv->lock, lock_flag);
	list_add_tail(&cb_data->list, &priv->cb_data.list);
	spin_unlock_irqrestore(&priv->lock, lock_flag);

	complete(&priv->wait_interrupt);

exit:
	/* release memory */
	if (entry_data->job.type == VSPM_TYPE_VSP_AUTO)
		free_vsp_par(&entry_data->ip_par.vsp);
	kfree(entry_data);
}

static long vspm_ioctl_entry(
	struct vspm_if_private_t *priv, unsigned int cmd, unsigned long arg)
{
	struct vspm_if_entry_data_t *entry_data;
	struct vspm_if_entry_req_t *entry_req;
	struct vspm_if_entry_rsp_t *entry_rsp;

	int ercd = 0;

	/* allocate entry data */
	entry_data = kzalloc(sizeof(struct vspm_if_entry_data_t), GFP_KERNEL);
	if (entry_data == NULL)
		return -ENOMEM;
	entry_data->priv = priv;

	/* copy entry parameter */
	if (copy_from_user(
			&entry_data->entry,
			(void __user *)arg,
			_IOC_SIZE(cmd))) {
		EPRINT("ENTRY: failed to copy the entry parameter\n");
		kfree(entry_data);
		return -EFAULT;
	}

	entry_req = &entry_data->entry.req;
	entry_rsp = &entry_data->entry.rsp;

	if (entry_req->job_param != NULL) {
		/* copy job parameter */
		if (copy_from_user(
				&entry_data->job,
				(void __user *)entry_req->job_param,
				sizeof(struct vspm_job_t))) {
			EPRINT("ENTRY: failed to copy the job parameter\n");
			kfree(entry_data);
			return -EFAULT;
		}
		entry_req->job_param = &entry_data->job;

		switch (entry_data->job.type) {
		case VSPM_TYPE_VSP_AUTO:
			if (entry_data->job.par.vsp) {
				/* copy start parameter of VSP */
				ercd = set_vsp_par(
					entry_data, entry_data->job.par.vsp);
				if (ercd) {
					kfree(entry_data);
					return ercd;
				}
				entry_req->job_param->par.vsp =
					&entry_data->ip_par.vsp.par;
			}
			break;
		case VSPM_TYPE_FDP_AUTO:
			if (entry_data->job.par.fdp) {
				/* copy start parameter of FDP */
				ercd = set_fdp_par(
					entry_data, entry_data->job.par.fdp);
				if (ercd) {
					kfree(entry_data);
					return ercd;
				}
				entry_req->job_param->par.fdp =
					&entry_data->ip_par.fdp.par;
			}
			break;
		default:
			break;
		}
	}

	/* entry job */
	entry_rsp->ercd = vspm_entry_job(
		priv->handle,
		&entry_rsp->job_id,
		entry_req->priority,
		entry_req->job_param,
		(unsigned long)entry_data,
		vspm_cb_func);

	/* copy result to user */
	if (copy_to_user(
			(void __user *)arg, &entry_data->entry, _IOC_SIZE(cmd)))
		APRINT("ENTRY: failed to copy the result\n");

	if (entry_rsp->ercd != R_VSPM_OK) {
		if (entry_data->job.type == VSPM_TYPE_VSP_AUTO)
			free_vsp_par(&entry_data->ip_par.vsp);
		kfree(entry_data);
	}

	return 0;
}

static long vspm_ioctl_cancel(
	struct vspm_if_private_t *priv, unsigned int cmd, unsigned long arg)
{
	unsigned long job_id = 0;
	long ercd;

	/* copy cancel parameter */
	if (copy_from_user(&job_id, (void __user *)arg, _IOC_SIZE(cmd))) {
		EPRINT("CANCEL: failed to copy the request data\n");
		return -EFAULT;
	}

	/* cancel job */
	ercd = vspm_cancel_job(priv->handle, job_id);
	switch (ercd) {
	case R_VSPM_OK:
		break;
	case VSPM_STATUS_ACTIVE:
		return -EBUSY;
		break;
	case VSPM_STATUS_NO_ENTRY:
		return -ENOENT;
		break;
	default:
		return -EFAULT;
		break;
	}

	return 0;
}

static long vspm_ioctl_get_status(
	struct vspm_if_private_t *priv, unsigned int cmd, unsigned long arg)
{
	struct vspm_status_t status;
	struct fdp_status_t fdp_status;

	long ercd;

	struct vspm_status_t user_status;

	/* copy status parameter from user */
	if (copy_from_user(&user_status, (void __user *)arg, _IOC_SIZE(cmd))) {
		EPRINT("GET: failed to copy from user\n");
		return -EFAULT;
	}

	status.fdp = &fdp_status;

	/* get a status */
	ercd = vspm_get_status(priv->handle, &status);
	switch (ercd) {
	case R_VSPM_OK:
		/* copy status parameter to user */
		if (copy_to_user(
				(void __user *)user_status.fdp,
				&fdp_status,
				sizeof(struct fdp_status_t))) {
			EPRINT("GET: failed to copy to user\n");
			return -EFAULT;
		}
		break;
	case R_VSPM_PARAERR:
		return -EINVAL;
		break;
	default:
		return -EFAULT;
		break;
	}

	return 0;
}

static long vspm_ioctl_wait_interrupt(
	struct vspm_if_private_t *priv, unsigned int cmd, unsigned long arg)
{
	long ercd = 0;

	/* get user process information */
	priv->thread = current;
	complete(&priv->wait_thread);

	/* wait process end */
	if (wait_for_completion_interruptible(&priv->wait_interrupt))
		return -EINTR;

	if (list_empty(&priv->cb_data.list)) {
		struct vspm_if_cb_rsp_t rsp;

		/* set response data (ercd = -1) */
		memset(&rsp, 0, sizeof(struct vspm_if_cb_rsp_t));
		rsp.ercd = -1;

		/* copy response data to user */
		if (copy_to_user((void __user *)arg, &rsp, _IOC_SIZE(cmd))) {
			EPRINT("CB: failed to copy the response\n");
			return -EFAULT;
		}
	} else {
		struct vspm_if_cb_data_t *cb_data;
		unsigned long lock_flag;

		/* get response data */
		spin_lock_irqsave(&priv->lock, lock_flag);
		cb_data = list_first_entry(
			&priv->cb_data.list, struct vspm_if_cb_data_t, list);
		list_del(&cb_data->list);
		spin_unlock_irqrestore(&priv->lock, lock_flag);

		/* HGO result */
		if (cb_data->vsp_hgo.knel_addr != NULL) {
			/* copy to user area */
			if (cb_data->vsp_hgo.user_addr != NULL) {
				if (copy_to_user((void __user *)
						cb_data->vsp_hgo.user_addr,
						cb_data->vsp_hgo.knel_addr,
						1088)) {
					APRINT("CB: failed to copy HGO data\n");
					ercd = -EFAULT;
				}
			}
			/* release memory */
			kfree(cb_data->vsp_hgo.knel_addr);
		}

		/* HGT result */
		if (cb_data->vsp_hgt.knel_addr != NULL) {
			/* copy to user area */
			if (cb_data->vsp_hgt.user_addr != NULL) {
				if (copy_to_user((void __user *)
						cb_data->vsp_hgt.user_addr,
						cb_data->vsp_hgt.knel_addr,
						800)) {
					APRINT("CB: failed to copy HGT data\n");
					ercd = -EFAULT;
				}
			}
			/* release memory */
			kfree(cb_data->vsp_hgt.knel_addr);
		}

		/* copy response data to user */
		if (copy_to_user(
				(void __user *)arg,
				&cb_data->rsp,
				_IOC_SIZE(cmd))) {
			EPRINT("CB: failed to copy the response\n");
			ercd = -EFAULT;
		}

		/* release memory */
		kfree(cb_data);
	}

	return ercd;
}

static long vspm_ioctl_wait_thread(struct vspm_if_private_t *priv)
{
	/* wait for callback thread of user */
	if (wait_for_completion_interruptible(&priv->wait_thread)) {
		APRINT("CB_START: INTR\n");
		return -EINTR;
	}

	/* wait for running state of callback thread  */
	while ((!priv->thread) || (priv->thread->state == TASK_RUNNING))
		schedule();

	return 0;
}

static long vspm_ioctl_stop_thread(struct vspm_if_private_t *priv)
{
	struct vspm_if_cb_data_t *cb_data;
	struct vspm_if_cb_data_t *next;

	unsigned long lock_flag;

	spin_lock_irqsave(&priv->lock, lock_flag);
	list_for_each_entry_safe(cb_data, next, &priv->cb_data.list, list) {
		list_del(&cb_data->list);
		if (cb_data->vsp_hgo.knel_addr != NULL)
			kfree(cb_data->vsp_hgo.knel_addr);
		if (cb_data->vsp_hgt.knel_addr != NULL)
			kfree(cb_data->vsp_hgt.knel_addr);
		kfree(cb_data);
	}
	spin_unlock_irqrestore(&priv->lock, lock_flag);

	complete(&priv->wait_interrupt);

	return 0;
}

static long unlocked_ioctl(
	struct file *file, unsigned int cmd, unsigned long arg)
{
	struct vspm_if_private_t *priv =
		(struct vspm_if_private_t *)file->private_data;

	long ercd = 0;

	/* check parameter */
	if (priv == NULL) {
		EPRINT("IOCTL: invalid private data!!\n");
		return -EFAULT;
	}

	switch (cmd) {
	case VSPM_IOC_CMD_INIT:
		ercd = vspm_ioctl_init(priv, cmd, arg);
		break;
	case VSPM_IOC_CMD_QUIT:
		ercd = vspm_ioctl_quit(priv);
		break;
	case VSPM_IOC_CMD_ENTRY:
		ercd = vspm_ioctl_entry(priv, cmd, arg);
		break;
	case VSPM_IOC_CMD_CANCEL:
		ercd = vspm_ioctl_cancel(priv, cmd, arg);
		break;
	case VSPM_IOC_CMD_GET_STATUS:
		ercd = vspm_ioctl_get_status(priv, cmd, arg);
		break;
	case VSPM_IOC_CMD_WAIT_INTERRUPT:
		ercd = vspm_ioctl_wait_interrupt(priv, cmd, arg);
		break;
	case VSPM_IOC_CMD_WAIT_THREAD:
		ercd = vspm_ioctl_wait_thread(priv);
		break;
	case VSPM_IOC_CMD_STOP_THREAD:
		ercd = vspm_ioctl_stop_thread(priv);
		break;
	default:
		ercd = -ENOTTY;
		break;
	}

	return ercd;
}

static long vspm_ioctl_init32(
	struct vspm_if_private_t *priv, unsigned int cmd, unsigned long arg)
{
	/* for 64bit */
	struct vspm_init_t init_par;
	struct vspm_init_fdp_t init_fdp_par;

	unsigned long handle;
	long ercd;

	/* for 32bit */
	struct vspm_compat_init_t compat_init_par;
	struct vspm_compat_init_fdp_t compat_init_fdp_par;

	/* copy initialize parameter */
	if (copy_from_user(
			&compat_init_par, (void __user *)arg, _IOC_SIZE(cmd))) {
		EPRINT("INIT32: failed to copy from user!!\n");
		return -EFAULT;
	}
	init_par.use_ch = compat_init_par.use_ch;
	init_par.mode = compat_init_par.mode;
	init_par.type = compat_init_par.type;

	switch (init_par.type) {
	case VSPM_TYPE_VSP_AUTO:
		/* initialize parameter is not used */
		init_par.par.vsp = NULL;
		break;
	case VSPM_TYPE_FDP_AUTO:
		/* copy initialize parameter of FDP */
		if (copy_from_user(
				&compat_init_fdp_par,
				VSPM_IF_INT_TO_UP(compat_init_par.par.fdp),
				sizeof(struct vspm_compat_init_fdp_t))) {
			EPRINT("INIT32: failed to copy FDP param\n");
			return -EFAULT;
		}
		init_fdp_par.hard_addr[0] =
			VSPM_IF_INT_TO_VP(compat_init_fdp_par.hard_addr[0]);
		init_fdp_par.hard_addr[1] =
			VSPM_IF_INT_TO_VP(compat_init_fdp_par.hard_addr[1]);
		init_par.par.fdp = &init_fdp_par;
		break;
	default:
		break;
	}

	/* initialize VSP manager */
	ercd = vspm_init_driver(&handle, &init_par);
	switch (ercd) {
	case R_VSPM_OK:
		break;
	case R_VSPM_PARAERR:
		return -EINVAL;
		break;
	case R_VSPM_ALREADY_USED:
		return -EBUSY;
		break;
	default:
		return -EFAULT;
		break;
	}

	priv->handle = handle;
	return 0;
}

static long vspm_ioctl_entry32(
	struct vspm_if_private_t *priv, unsigned int cmd, unsigned long arg)
{
	/* for 64bit */
	struct vspm_if_entry_data_t *entry_data;
	struct vspm_if_entry_req_t *entry_req;
	struct vspm_if_entry_rsp_t *entry_rsp;

	/* for 32bit */
	struct vspm_compat_entry_t compat_entry;
	struct vspm_compat_job_t compat_job;
	struct vspm_compat_entry_req_t *compat_req = &compat_entry.req;
	struct vspm_compat_entry_rsp_t *compat_rsp = &compat_entry.rsp;

	int ercd = 0;

	/* allocate entry data */
	entry_data = kzalloc(sizeof(struct vspm_if_entry_data_t), GFP_KERNEL);
	if (entry_data == NULL)
		return -ENOMEM;
	entry_data->priv = priv;

	entry_req = &entry_data->entry.req;
	entry_rsp = &entry_data->entry.rsp;

	/* copy entry parameter */
	if (copy_from_user(
			&compat_entry, (void __user *)arg, _IOC_SIZE(cmd))) {
		EPRINT("ENTRY32: failed to copy the entry parameter\n");
		kfree(entry_data);
		return -EFAULT;
	}

	entry_req->priority = compat_req->priority;
	entry_req->user_data = (unsigned long)compat_req->user_data;
	entry_req->cb_func = VSPM_IF_INT_TO_CP(compat_req->cb_func);

	if (compat_req->job_param != 0) {
		/* copy job parameter */
		if (copy_from_user(
				&compat_job,
				VSPM_IF_INT_TO_UP(compat_req->job_param),
				sizeof(struct vspm_compat_job_t))) {
			EPRINT("ENTRY32: failed to copy the job parameter\n");
			kfree(entry_data);
			return -EFAULT;
		}
		entry_data->job.type = compat_job.type;

		switch (compat_job.type) {
		case VSPM_TYPE_VSP_AUTO:
			/* copy start parameter of VSP */
			if (compat_job.par.vsp) {
				ercd = set_compat_vsp_par(
					entry_data, compat_job.par.vsp);
				if (ercd) {
					kfree(entry_data);
					return ercd;
				}
				entry_data->job.par.vsp =
					&entry_data->ip_par.vsp.par;
			}
			break;
		case VSPM_TYPE_FDP_AUTO:
			/* copy start parameter of FDP */
			if (compat_job.par.fdp) {
				ercd = set_compat_fdp_par(
					entry_data, compat_job.par.fdp);
				if (ercd) {
					kfree(entry_data);
					return ercd;
				}
				entry_data->job.par.fdp =
					&entry_data->ip_par.fdp.par;
			}
			break;
		default:
			break;
		}

		entry_req->job_param = &entry_data->job;
	}

	/* entry job */
	entry_rsp->ercd = vspm_entry_job(
		priv->handle,
		&entry_rsp->job_id,
		entry_req->priority,
		entry_req->job_param,
		(unsigned long)entry_data,
		vspm_cb_func);

	/* copy result to user */
	compat_rsp->ercd = (int)entry_rsp->ercd;
	compat_rsp->job_id = (unsigned int)entry_rsp->job_id;
	if (copy_to_user(
			(void __user *)arg, &compat_entry, _IOC_SIZE(cmd))) {
		APRINT("ENTRY32: failed to copy the result\n");
	}

	if (entry_rsp->ercd != R_VSPM_OK) {
		if (entry_data->job.type == VSPM_TYPE_VSP_AUTO)
			free_vsp_par(&entry_data->ip_par.vsp);
		kfree(entry_data);
	}

	return 0;
}

static long vspm_ioctl_get_status32(
	struct vspm_if_private_t *priv, unsigned int cmd, unsigned long arg)
{
	/* for 64bit */
	struct vspm_status_t status;
	struct fdp_status_t fdp_status;

	long ercd;

	/* for 32bit */
	struct vspm_compat_status_t compat_status;
	struct compat_fdp_status_t compat_fdp_status;

	int i;

	/* copy status parameter from user */
	if (copy_from_user(
			&compat_status, (void __user *)arg, _IOC_SIZE(cmd))) {
		EPRINT("GET32: failed to copy from user\n");
		return -EFAULT;
	}

	status.fdp = &fdp_status;

	/* get a status */
	ercd = vspm_get_status(priv->handle, &status);
	switch (ercd) {
	case R_VSPM_OK:
		/* convert parameter */
		compat_fdp_status.picid = (unsigned int)fdp_status.picid;
		compat_fdp_status.vcycle = fdp_status.vcycle;
		for (i = 0; i < 18; i++)
			compat_fdp_status.sensor[i] = fdp_status.sensor[i];

		/* copy status parameter to user */
		if (copy_to_user(
				VSPM_IF_INT_TO_UP(compat_status.fdp),
				&compat_fdp_status,
				sizeof(struct compat_fdp_status_t))) {
			EPRINT("GET32: failed to copy to user\n");
			return -EFAULT;
		}
		break;
	case R_VSPM_PARAERR:
		return -EINVAL;
		break;
	default:
		return -EFAULT;
		break;
	}

	return 0;
}

static long vspm_ioctl_wait_interrupt32(
	struct vspm_if_private_t *priv, unsigned int cmd, unsigned long arg)
{
	long ercd = 0;

	/* for 32bit */
	struct vspm_compat_cb_rsp_t compat_rsp;

	/* get user process information */
	priv->thread = current;
	complete(&priv->wait_thread);

	/* wait process end */
	if (wait_for_completion_interruptible(
			&priv->wait_interrupt)) {
		return -EINTR;
	}

	if (list_empty(&priv->cb_data.list)) {
		/* set response data (ercd = -1) */
		memset(&compat_rsp, 0, sizeof(struct vspm_compat_cb_rsp_t));
		compat_rsp.ercd = -1;

		/* copy response data to user */
		if (copy_to_user(
				(void __user *)arg,
				&compat_rsp,
				_IOC_SIZE(cmd))) {
			EPRINT("CB32: failed to copy the response\n");
			return -EFAULT;
		}
	} else {
		struct vspm_if_cb_data_t *cb_data;
		unsigned long lock_flag;

		/* get response data */
		spin_lock_irqsave(&priv->lock, lock_flag);
		cb_data = list_first_entry(
			&priv->cb_data.list, struct vspm_if_cb_data_t, list);
		list_del(&cb_data->list);
		spin_unlock_irqrestore(&priv->lock, lock_flag);

		compat_rsp.ercd = (int)cb_data->rsp.ercd;
		compat_rsp.cb_func = VSPM_IF_CP_TO_INT(cb_data->rsp.cb_func);
		compat_rsp.result = (int)cb_data->rsp.result;
		compat_rsp.user_data = (unsigned int)cb_data->rsp.user_data;

		/* copy response data to user */
		if (copy_to_user(
				(void __user *)arg,
				&compat_rsp,
				_IOC_SIZE(cmd))) {
			EPRINT("CB32: failed to copy the response\n");
			ercd = -EFAULT;
		}

		/* release memory */
		kfree(cb_data);
	}

	return ercd;
}

static long compat_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct vspm_if_private_t *priv =
		(struct vspm_if_private_t *)file->private_data;

	long ercd = 0;

	/* check parameter */
	if (priv == NULL) {
		EPRINT("IOCTL32: invalid private data!!\n");
		return -EFAULT;
	}

	switch (cmd) {
	case VSPM_IOC_CMD_INIT32:
		ercd = vspm_ioctl_init32(priv, cmd, arg);
		break;
	case VSPM_IOC_CMD_QUIT:
		ercd = vspm_ioctl_quit(priv);
		break;
	case VSPM_IOC_CMD_ENTRY32:
		ercd = vspm_ioctl_entry32(priv, cmd, arg);
		break;
	case VSPM_IOC_CMD_CANCEL32:
		ercd = vspm_ioctl_cancel(priv, cmd, arg);
		break;
	case VSPM_IOC_CMD_GET_STATUS32:
		ercd = vspm_ioctl_get_status32(priv, cmd, arg);
		break;
	case VSPM_IOC_CMD_WAIT_INTERRUPT32:
		ercd = vspm_ioctl_wait_interrupt32(priv, cmd, arg);
		break;
	case VSPM_IOC_CMD_WAIT_THREAD:
		ercd = vspm_ioctl_wait_thread(priv);
		break;
	case VSPM_IOC_CMD_STOP_THREAD:
		ercd = vspm_ioctl_stop_thread(priv);
		break;
	default:
		ercd = -ENOTTY;
		break;
	}

	return ercd;
}

static const struct file_operations fops = {
	.owner   = THIS_MODULE,
	.open    = open,
	.release = close,
	.unlocked_ioctl = unlocked_ioctl,
	.compat_ioctl = compat_ioctl,
};

static struct miscdevice misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = DEVFILE,
	.fops = &fops
};

static int vspm_if_init(void)
{
	misc_register(&misc);
	dev = misc.this_device;
	dev->coherent_dma_mask = ~0;

	return 0;
}

static void vspm_if_exit(void)
{
	misc_deregister(&misc);
}

module_init(vspm_if_init);
module_exit(vspm_if_exit);

MODULE_AUTHOR("Renesas Electronics Corporation");
MODULE_LICENSE("Dual MIT/GPL");
