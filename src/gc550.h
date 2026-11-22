/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * AVerMedia Live Gamer Extreme (GC550) USB capture driver.
 */
#ifndef GC550_H
#define GC550_H

#include <linux/kref.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/scatterlist.h>
#include <linux/spinlock.h>
#include <linux/usb.h>
#include <linux/workqueue.h>
#include <media/v4l2-device.h>
#include <media/videobuf2-v4l2.h>

#include "gc550-parser.h"
#include "gc550-signal.h"

#define GC550_DRIVER_NAME	"gc550"

/* Endpoints of the card's single vendor-class interface. */
#define GC550_EP_CMD_OUT	0x01	/* command bridge, see gc550-guard.h */
#define GC550_EP_CMD_IN		0x81	/* bridge replies */
#define GC550_EP_STREAM_IN	0x83	/* video + audio stream */

/* Largest bridge reply: the 512-byte board-info read. */
#define GC550_BRIDGE_BUF	512
#define GC550_BRIDGE_TIMEOUT_MS	1000

/*
 * Stream transfers: the same size lgx2userspace uses (proven on hardware),
 * each built from 64 KiB chunks in a scatter-gather list so no 2 MiB
 * contiguous allocation is needed.
 */
#define GC550_URBS		4
#define GC550_URB_BYTES		0x1FC000
#define GC550_CHUNK_ORDER	4
#define GC550_CHUNK_BYTES	(PAGE_SIZE << GC550_CHUNK_ORDER)
#define GC550_URB_CHUNKS	DIV_ROUND_UP(GC550_URB_BYTES, GC550_CHUNK_BYTES)
#define GC550_RX_MAX_ERRORS	16

#define GC550_AUDIO_RATE	48000
#define GC550_AUDIO_CHANNELS	2
#define GC550_AUDIO_FRAME_BYTES	(GC550_AUDIO_CHANNELS * 2)

struct gc550_dev;

struct gc550_urb {
	struct gc550_dev *dev;
	struct urb *urb;
	struct list_head done;		/* on gc550_dev.rx_done after completion */
	struct page *chunks[GC550_URB_CHUNKS];
	struct scatterlist sg[GC550_URB_CHUNKS];
};

struct gc550_buffer {
	struct vb2_v4l2_buffer vb;	/* must be first */
	struct list_head list;
};

enum gc550_user {
	GC550_USER_VIDEO = BIT(0),
	GC550_USER_AUDIO = BIT(1),
};

struct gc550_dev {
	struct kref kref;
	struct usb_device *udev;
	struct usb_interface *intf;
	bool disconnected;		/* set under bridge_lock */

	/* command bridge: the only path to GC550_EP_CMD_OUT */
	struct mutex bridge_lock;
	u8 *bridge_buf;			/* DMA-able, GC550_BRIDGE_BUF bytes */

	/* bootstrap and registration, on system_wq */
	struct work_struct boot_work;
	bool v4l2_registered;
	bool snd_registered;

	/* input status poll */
	struct delayed_work status_work;
	struct mutex status_lock;	/* protects signal, signal_valid */
	struct gc550_signal signal;
	bool signal_valid;
	unsigned int status_failures;

	/* stream (IN 0x83), shared by video and audio */
	struct mutex stream_lock;	/* protects users, urbs allocation */
	unsigned int users;		/* enum gc550_user bits */
	struct gc550_urb urbs[GC550_URBS];
	bool urbs_allocated;
	struct usb_anchor anchor;
	struct workqueue_struct *rx_wq;
	struct work_struct rx_work;
	spinlock_t rx_lock;		/* protects rx_done, streaming */
	struct list_head rx_done;
	bool streaming;
	bool gap;			/* only touched by rx_work */
	unsigned int rx_errors;		/* consecutive failed transfers; rx_work */
	struct mutex parse_lock;	/* held while the parser runs */
	struct gc550_parser parser;

	/* V4L2 */
	struct v4l2_device v4l2_dev;
	struct video_device vdev;
	struct vb2_queue queue;
	struct mutex vb_lock;		/* serialises ioctls and the queue */
	spinlock_t buf_lock;		/* protects bufs */
	struct list_head bufs;
	struct gc550_buffer *cur;	/* held by the parser; under parse_lock */
	bool video_on;			/* under parse_lock */
	u32 sequence;
	u64 video_dropped;		/* while streaming, for want of a buffer */

	/* ALSA */
	struct snd_card *card;
	struct snd_pcm_substream *substream;
	spinlock_t audio_lock;		/* protects the fields below */
	bool audio_on;
	size_t audio_pos;		/* write position in the ring, bytes */
	size_t audio_period_fill;	/* bytes since the last period */
	bool audio_elapsed;		/* a period completed, not yet reported */
	bool audio_hw;			/* holds a stream reference */
};

/* gc550-core.c */
void gc550_dev_put(struct gc550_dev *dev);
void gc550_dev_get(struct gc550_dev *dev);

/* gc550-bridge.c */
int gc550_bridge_send(struct gc550_dev *dev, const u8 *cmd, size_t len, u8 *reply);
int gc550_i2c_read(struct gc550_dev *dev, u8 addr, u8 reg, u8 *out, u8 count);
int gc550_i2c_write(struct gc550_dev *dev, u8 addr, u8 reg, u8 val);
void gc550_bridge_drain(struct gc550_dev *dev);

/* gc550-boot.c */
int gc550_boot(struct gc550_dev *dev);

/* gc550-status.c */
void gc550_status_init(struct gc550_dev *dev);
int gc550_status_read(struct gc550_dev *dev, struct gc550_signal *s);
void gc550_status_start(struct gc550_dev *dev);
void gc550_status_stop(struct gc550_dev *dev);
bool gc550_input_protected(struct gc550_dev *dev);

/* gc550-stream.c */
int gc550_stream_init(struct gc550_dev *dev);
void gc550_stream_cleanup(struct gc550_dev *dev);
int gc550_stream_get(struct gc550_dev *dev, enum gc550_user user);
void gc550_stream_put(struct gc550_dev *dev, enum gc550_user user);
void gc550_stream_disconnect(struct gc550_dev *dev);

/* gc550-v4l2.c */
int gc550_v4l2_register(struct gc550_dev *dev);
void gc550_v4l2_unregister(struct gc550_dev *dev);
void gc550_v4l2_source_change(struct gc550_dev *dev);
u8 *gc550_video_frame_buffer(struct gc550_dev *dev, size_t *capacity);
void gc550_video_frame_done(struct gc550_dev *dev, size_t bytes);
void gc550_video_frame_dropped(struct gc550_dev *dev);
void gc550_video_release_buffer(struct gc550_dev *dev);

/* gc550-alsa.c */
int gc550_alsa_register(struct gc550_dev *dev);
void gc550_alsa_unregister(struct gc550_dev *dev);
void gc550_audio_data(struct gc550_dev *dev, const u8 *data, size_t len);
void gc550_audio_flush_periods(struct gc550_dev *dev);

#endif /* GC550_H */
