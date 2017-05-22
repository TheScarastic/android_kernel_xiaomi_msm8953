/*
 *  Copyright (C) 2010,Imagis Technology Co. Ltd. All Rights Reserved.
 * Copyright (C) 2017 XiaoMi, Inc.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 */

#include <linux/i2c.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/stat.h>

#include "ist30xxc.h"
#ifdef IST30XX_TRACKING_MODE
#include "ist30xxc_tracking.h"
#endif

#ifdef IST30XX_TRACKING_MODE
IST30XX_RING_BUF TrackBuf;
IST30XX_RING_BUF *pTrackBuf;

bool tracking_initialize = false;

void ist30xx_tracking_init(void)
{
	if (tracking_initialize)
		return;

	pTrackBuf = &TrackBuf;

	pTrackBuf->RingBufCtr = 0;
	pTrackBuf->RingBufInIdx = 0;
	pTrackBuf->RingBufOutIdx = 0;

	tracking_initialize = true;
}

void ist30xx_tracking_deinit(void)
{
}

static spinlock_t mr_lock = __SPIN_LOCK_UNLOCKED();
int ist30xx_get_track(u32 *track, int cnt)
{
	int i;
	u8 *buf = (u8 *)track;
	unsigned long flags;

	cnt *= sizeof(track[0]);

	spin_lock_irqsave(&mr_lock, flags);

	if (pTrackBuf->RingBufCtr < (u16)cnt) {
		spin_unlock_irqrestore(&mr_lock, flags);
		return IST30XX_RINGBUF_NOT_ENOUGH;
	}

	for (i = 0; i < cnt; i++) {
		if (pTrackBuf->RingBufOutIdx == IST30XX_MAX_RINGBUF_SIZE)
			pTrackBuf->RingBufOutIdx = 0;

		*buf++ = (u8)pTrackBuf->LogBuf[pTrackBuf->RingBufOutIdx++];
		pTrackBuf->RingBufCtr--;
	}

	spin_unlock_irqrestore(&mr_lock, flags);

	return IST30XX_RINGBUF_NO_ERR;
}

u32 ist30xx_get_track_cnt(void)
{
	return pTrackBuf->RingBufCtr;
}

int ist30xx_put_track(u32 *track, int cnt)
{
	int i;
	u8 *buf = (u8 *)track;
	unsigned long flags;

	spin_lock_irqsave(&mr_lock, flags);

	cnt *= sizeof(track[0]);

	pTrackBuf->RingBufCtr += cnt;
	if (pTrackBuf->RingBufCtr > IST30XX_MAX_RINGBUF_SIZE) {
		pTrackBuf->RingBufOutIdx +=
			(pTrackBuf->RingBufCtr - IST30XX_MAX_RINGBUF_SIZE);
		if (pTrackBuf->RingBufOutIdx >= IST30XX_MAX_RINGBUF_SIZE)
			pTrackBuf->RingBufOutIdx -= IST30XX_MAX_RINGBUF_SIZE;

		pTrackBuf->RingBufCtr = IST30XX_MAX_RINGBUF_SIZE;
	}

	for (i = 0; i < cnt; i++) {
		if (pTrackBuf->RingBufInIdx == IST30XX_MAX_RINGBUF_SIZE)
			pTrackBuf->RingBufInIdx = 0;
		pTrackBuf->LogBuf[pTrackBuf->RingBufInIdx++] = *buf++;
	}

	spin_unlock_irqrestore(&mr_lock, flags);

	return IST30XX_RINGBUF_NO_ERR;
}

int ist30xx_put_track_ms(u32 ms)
{
	ms &= 0x0000FFFF;
	ms |= IST30XX_TRACKING_MAGIC;

	return ist30xx_put_track(&ms, 1);
}

static struct timespec t_track;
int ist30xx_tracking(u32 status)
{
	u32 ms;

	if (!tracking_initialize)
		ist30xx_tracking_init();

	ktime_get_ts(&t_track);
	ms = t_track.tv_sec * 1000 + t_track.tv_nsec / 1000000;

	ist30xx_put_track_ms(ms);
	ist30xx_put_track(&status, 1);

	return 0;
}

#define MAX_TRACKING_COUNT      (1024)
/* sysfs: /sys/class/touch/tracking/track_frame */
ssize_t ist30xx_track_frame_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	int i, buf_cnt = 0;
	u32 track_cnt = MAX_TRACKING_COUNT;
	u32 track;
	char msg[10];
	struct ist30xx_data *data = dev_get_drvdata(dev);

	mutex_lock(&data->lock);

	buf[0] = '\0';

	if (track_cnt > ist30xx_get_track_cnt())
		track_cnt = ist30xx_get_track_cnt();

	track_cnt /= sizeof(track);

	tsp_verb("num: %d of %d\n", track_cnt, ist30xx_get_track_cnt());

	for (i = 0; i < track_cnt; i++) {
		ist30xx_get_track(&track, 1);

		tsp_verb("%08X\n", track);

		buf_cnt += sprintf(msg, "%08x", track);
		strcat(buf, msg);
	}

	mutex_unlock(&data->lock);

	return buf_cnt;
}

/* sysfs: /sys/class/touch/tracking/track_cnt */
ssize_t ist30xx_track_cnt_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	u32 val = (u32)ist30xx_get_track_cnt();

	tsp_verb("tracking cnt: %d\n", val);

	return sprintf(buf, "%08x", val);
}


/* sysfs  */
static DEVICE_ATTR(track_frame, 0664, ist30xx_track_frame_show, NULL);
static DEVICE_ATTR(track_cnt, 0664, ist30xx_track_cnt_show, NULL);

static struct attribute *tracking_attributes[] = {
	&dev_attr_track_frame.attr,
	&dev_attr_track_cnt.attr,
	NULL,
};

static struct attribute_group tracking_attr_group = {
	.attrs    = tracking_attributes,
};

extern struct class *ist30xx_class;
struct device *ist30xx_tracking_dev;

int ist30xx_init_tracking_sysfs(struct ist30xx_data *data)
{
	/* /sys/class/touch/tracking */
	ist30xx_tracking_dev = device_create(ist30xx_class, NULL, 0, data,
					     "tracking");

	dev_set_drvdata(ist30xx_tracking_dev, data);
	/* /sys/class/touch/tracking/... */
	if (sysfs_create_group(&ist30xx_tracking_dev->kobj, &tracking_attr_group))
		tsp_err("[ TSP ] Failed to create sysfs group(%s)!\n", "tracking");

	ist30xx_tracking_init();

	return 0;
}
#endif
