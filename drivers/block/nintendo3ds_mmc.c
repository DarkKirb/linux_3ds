/*
 * nintendo3ds_mmc.c
 *
 *  Copyright (C) 2016 Sergi Granell <xerpi.g.12@gmail.com>
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
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <linux/sched.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/types.h>
#include <linux/genhd.h>
#include <linux/blkdev.h>
#include <linux/errno.h>

#include <mach/pxi.h>

#define NINTENDO3DS_MMC_BLOCKSIZE 512
#define NINTENDO3DS_MMC_FIRST_MINOR 0
#define NINTENDO3DS_MMC_MINOR_CNT 16

/****************************************************************************/

struct nintendo3ds_mmc {
	int major;
	/* Size is the size of the device (in sectors) */
	unsigned int size;
	/* For exclusive access to our request queue */
	spinlock_t lock;
	/* Our request queue */
	struct request_queue *queue;
	/* This is kernel's representation of an individual disk device */
	struct gendisk *disk;
};

static void nintendo3ds_pxi_mmc_write_sectors(struct nintendo3ds_mmc *mmc,
	sector_t sector_off, u8 *buffer, unsigned int sectors)
{
    unsigned int i;

    for (i = 0; i < sectors; i++) {
        struct pxi_cmd_sdmmc_write_sector cmd = {
            .header.cmd = PXI_CMD_SDMMC_WRITE_SECTOR,
            .header.len = sizeof(cmd) - sizeof(struct pxi_cmd_hdr),
            .sector = sector_off + i,
            .paddr = (u32)virt_to_phys(buffer + i * NINTENDO3DS_MMC_BLOCKSIZE)
        };

        pxi_send_cmd((struct pxi_cmd_hdr *)&cmd, 1 /* wait for response */);
    }
}

static void nintendo3ds_pxi_mmc_read_sectors(struct nintendo3ds_mmc *mmc,
	sector_t sector_off, const u8 *buffer, unsigned int sectors)
{
	unsigned int i;

	for (i = 0; i < sectors; i++) {
		struct pxi_cmd_sdmmc_read_sector cmd = {
			.header.cmd = PXI_CMD_SDMMC_READ_SECTOR,
			.header.len = sizeof(cmd) - sizeof(struct pxi_cmd_hdr),
			.sector = sector_off + i,
			.paddr = (u32)virt_to_phys(buffer + i * NINTENDO3DS_MMC_BLOCKSIZE)
		};

		pxi_send_cmd((struct pxi_cmd_hdr *)&cmd, 1 /* wait for response */);
	}
}

static int nintendo3ds_mmc_xfer_request(struct nintendo3ds_mmc *mmc, struct request *req)
{
	struct bio_vec bvec;
	struct req_iterator iter;
	sector_t sector_offset;
	unsigned int sectors;
	u8 *buffer;
	int ret = 0;
	const int dir = rq_data_dir(req);
	const sector_t start_sector = blk_rq_pos(req);
	const unsigned int sector_cnt = blk_rq_sectors(req);

	sector_offset = 0;

	rq_for_each_segment(bvec, req, iter) {
		buffer = page_address(bvec.bv_page) + bvec.bv_offset;
		sectors = bvec.bv_len / NINTENDO3DS_MMC_BLOCKSIZE;

		/*printk("n3ds MMC: start sect: %lld, sect off: %lld; buffer: %p; num sects: %u\n",
			start_sector, sector_offset, buffer, sectors);*/

		if (dir == READ)
			nintendo3ds_pxi_mmc_read_sectors(mmc,
				start_sector + sector_offset, buffer,
				sectors);
		else
			nintendo3ds_pxi_mmc_write_sectors(mmc,
				start_sector + sector_offset, buffer,
				sectors);

		sector_offset += sectors;
	}

	if (sector_offset != sector_cnt) {
		printk(KERN_ERR "n3ds MMC: bio info doesn't match with the request info");
		ret = -EIO;
	}

	return ret;
}

static void nintendo3ds_mmc_request(struct request_queue *q)
{
	struct nintendo3ds_mmc *mmc;
	struct request *req;
	int ret;

	while ((req = blk_fetch_request(q)) != NULL) {
		mmc = req->rq_disk->private_data;

		if (blk_rq_is_passthrough(req)) {
			printk(KERN_NOTICE "Skip non-fs request\n");
			__blk_end_request_all(req, -EIO);
			continue;
		}

		ret = nintendo3ds_mmc_xfer_request(mmc, req);
		__blk_end_request_all(req, ret);
	}

}

static int nintendo3ds_mmc_open(struct block_device *bdev, fmode_t mode)
{
	unsigned unit = iminor(bdev->bd_inode);

	if (unit > NINTENDO3DS_MMC_MINOR_CNT)
		return -ENODEV;

	return 0;
}

static void nintendo3ds_mmc_release(struct gendisk *disk, fmode_t mode)
{

}

static struct block_device_operations nintendo3ds_mmc_fops = {
	.owner = THIS_MODULE,
	.open = nintendo3ds_mmc_open,
	.release = nintendo3ds_mmc_release,
};

static int nintendo3ds_mmc_probe(struct platform_device *pdev)
{
	int error;
	struct nintendo3ds_mmc *mmc;
	struct pxi_cmd_hdr size_cmd;

	if(!pxi_is_active())
	{
		return -EPROBE_DEFER;
	}

	mmc = kzalloc(sizeof(*mmc), GFP_KERNEL);
	if (!mmc)
		return -ENOMEM;

	mmc->major = register_blkdev(0, "nintendo3ds_mmc");
	if (mmc->major <= 0) {
		error = -EBUSY;
		goto error_reg_blkdev;
	}

	printk("nintendo3ds_mmc: major: %d\n", mmc->major);

	spin_lock_init(&mmc->lock);

	mmc->queue = blk_init_queue(nintendo3ds_mmc_request, &mmc->lock);
	if (!mmc->queue) {
		error = -ENOMEM;
		goto error_init_queue;
	}

	mmc->disk = alloc_disk(NINTENDO3DS_MMC_MINOR_CNT);
	if (!mmc->disk) {
		error = -ENOMEM;
		goto error_alloc_disk;
	}

	mmc->disk->major = mmc->major;
	mmc->disk->first_minor = NINTENDO3DS_MMC_FIRST_MINOR;
	mmc->disk->fops = &nintendo3ds_mmc_fops;
	mmc->disk->private_data = mmc;
	mmc->disk->queue = mmc->queue;
	sprintf(mmc->disk->disk_name, "nintendo3ds_mmc");

	size_cmd.cmd = PXI_CMD_SDMMC_GET_SIZE;
	size_cmd.len = 0;

	pxi_send_cmd(&size_cmd, 0 /* don't wait for response - we handle it in the irq */);

	mmc->size = 0;
	while(mmc->size == 0)
		mmc->size = pxi_get_sdmmc_size();

	platform_set_drvdata(pdev, mmc);

	add_disk(mmc->disk);

	set_capacity(mmc->disk, mmc->size);

	dev_info(&pdev->dev, "Nintendo 3ds PXI SD/MMC %d\n",
			mmc->major);

	return 0;

error_alloc_disk:
	blk_cleanup_queue(mmc->queue);
error_init_queue:
	unregister_blkdev(mmc->major, "nintendo3ds_mmc");
error_reg_blkdev:
	kfree(mmc);

	return error;
}

static int __exit nintendo3ds_mmc_remove(struct platform_device *pdev)
{
	struct nintendo3ds_mmc *mmc = platform_get_drvdata(pdev);

	if (mmc) {
		del_gendisk(mmc->disk);
		put_disk(mmc->disk);
		blk_cleanup_queue(mmc->queue);
		unregister_blkdev(mmc->major, "nintendo3ds_mmc");
	}

	return 0;
}

static const struct of_device_id nintendo3ds_mmc_dt_ids[] = {
	{ .compatible = "nintendo3ds-mmc", },
	{},
};
MODULE_DEVICE_TABLE(of, nintendo3ds_mmc_dt_ids);

static struct platform_driver nintendo3ds_mmc_driver = {
	.driver		= {
		.name	= "nintendo3ds_mmc",
		.owner = THIS_MODULE,
		.of_match_table = nintendo3ds_mmc_dt_ids,
	},
	.remove		= __exit_p(nintendo3ds_mmc_remove),
};

module_platform_driver_probe(nintendo3ds_mmc_driver, nintendo3ds_mmc_probe);

MODULE_AUTHOR("Sergi Granell");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Nintendo 3DS PXI SD/MMC driver");
MODULE_ALIAS("platform:nintendo3ds_mmc");
