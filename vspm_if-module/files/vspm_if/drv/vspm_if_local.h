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

#ifndef __VSPM_IF_LOCAL_H__
#define __VSPM_IF_LOCAL_H__

#include <linux/sched.h>

extern struct device *dev;

/* define macro */
#define IPRINT(fmt, args...) \
	printk(KERN_INFO "vspm:%d: " fmt, current->pid, ##args)
#define APRINT(fmt, args...) \
	printk(KERN_ALERT "vspm:%d: " fmt, current->pid, ##args)
#define EPRINT(fmt, args...) \
	printk(KERN_ERR "vspm:%d: " fmt, current->pid, ##args)

/* entry data structure */
struct vspm_if_entry_data_t {
	struct vspm_if_private_t *priv;
	struct vspm_if_entry_t entry;
	struct vspm_job_t job;
	union {
		struct vspm_entry_vsp {
			/* parameter to VSP processing */
			struct vsp_start_t par;
			/* input image settings */
			struct vspm_entry_vsp_in {
				struct vsp_src_t in;
				struct vspm_entry_vsp_in_clut {
					struct vsp_dl_t clut;
					dma_addr_t hard_addr;
					void *virt_addr;
				} clut;
				struct vspm_entry_vsp_in_alpha {
					struct vsp_alpha_unit_t alpha;
					struct vsp_irop_unit_t irop;
					struct vsp_ckey_unit_t ckey;
					struct vsp_mult_unit_t mult;
				} alpha;
			} in[5];
			/* output image settings */
			struct vspm_entry_vsp_out {
				struct vsp_dst_t out;
				struct fcp_info_t fcp;
			} out;
			/* conversion processing settings */
			struct vspm_entry_vsp_ctrl {
				struct vsp_ctrl_t ctrl;
				struct vspm_entry_vsp_bru {
					struct vsp_bru_t bru;
					struct vsp_bld_dither_t dither_unit[5];
					struct vsp_bld_vir_t blend_virtual;
					struct vsp_bld_ctrl_t blend_unit[5];
					struct vsp_bld_rop_t rop_unit;
				} bru;
				struct vsp_sru_t sru;
				struct vsp_uds_t uds;
				struct vsp_lut_t lut;
				struct vsp_clu_t clu;
				struct vsp_hst_t hst;
				struct vsp_hsi_t hsi;
				struct vspm_entry_vsp_hgo {
					struct vsp_hgo_t hgo;
					dma_addr_t hard_addr;
					void *virt_addr;
					void *user_addr;
				} hgo;
				struct vspm_entry_vsp_hgt {
					struct vsp_hgt_t hgt;
					dma_addr_t hard_addr;
					void *virt_addr;
					void *user_addr;
				} hgt;
				struct vsp_shp_t shp;
				struct vsp_drc_t drc;
			} ctrl;
			/* display list settings */
			struct vspm_entry_vsp_dl {
				dma_addr_t hard_addr;
				void *virt_addr;
			} dl;
		} vsp;
		struct vspm_entry_fdp {
			/* parameter to FDP processing */
			struct fdp_start_t par;
			struct vspm_entry_fdp_fproc {
				struct fdp_fproc_t fproc;
				struct fdp_seq_t seq;
				struct fdp_pic_t in_pic;
				struct fdp_imgbuf_t out_buf;
				struct vspm_entry_fdp_ref {
					struct fdp_refbuf_t ref_buf;
					struct fdp_imgbuf_t ref[3];
				} ref;
				struct fcp_info_t fcp;
			} fproc;
		} fdp;
	} ip_par;
};

/* callback data structure */
struct vspm_if_cb_data_t {
	struct list_head list;
	struct vspm_if_cb_rsp_t rsp;
	struct vspm_cb_vsp_hgo {
		void *user_addr;
		void *knel_addr;
	} vsp_hgo;
	struct vspm_cb_vsp_hgt {
		void *user_addr;
		void *knel_addr;
	} vsp_hgt;
};

/* private data structure */
struct vspm_if_private_t {
	spinlock_t lock;
	struct task_struct *thread;
	struct vspm_if_cb_data_t cb_data;
	struct completion wait_interrupt;
	struct completion wait_thread;
	unsigned long handle;
};

/* sub function */
int free_vsp_par(struct vspm_entry_vsp *vsp);
int set_vsp_par(
	struct vspm_if_entry_data_t *entry,
	struct vsp_start_t *vsp_par);
void set_cb_rsp_vsp(
	struct vspm_if_cb_data_t *cb_data,
	struct vspm_if_entry_data_t *entry_data);
int set_fdp_par(
	struct vspm_if_entry_data_t *entry,
	struct fdp_start_t *fdp_par);

int set_compat_vsp_par(struct vspm_if_entry_data_t *entry, unsigned int *src);
int set_compat_fdp_par(struct vspm_if_entry_data_t *entry, unsigned int *src);

#endif /* __VSPM_IF_LOCAL_H__ */

