// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * AVerMedia Live Gamer Extreme (GC550) V4L2 capture device.
 *
 * The card always delivers 1920x1080 YUYV; that is the only format offered.
 */
#include <linux/string.h>
#include <media/v4l2-event.h>
#include <media/v4l2-ioctl.h>
#include <media/videobuf2-vmalloc.h>

#include "gc550.h"

#define GC550_BYTESPERLINE	(GC550_FRAME_WIDTH * 2)
#define GC550_FPS		60

static void gc550_fill_pix(struct v4l2_pix_format *pix)
{
	memset(pix, 0, sizeof(*pix));
	pix->width = GC550_FRAME_WIDTH;
	pix->height = GC550_FRAME_HEIGHT;
	pix->pixelformat = V4L2_PIX_FMT_YUYV;
	pix->field = V4L2_FIELD_NONE;
	pix->bytesperline = GC550_BYTESPERLINE;
	pix->sizeimage = GC550_FRAME_BYTES;
	pix->colorspace = V4L2_COLORSPACE_REC709;
	pix->ycbcr_enc = V4L2_YCBCR_ENC_709;
	pix->quantization = V4L2_QUANTIZATION_LIM_RANGE;
	pix->xfer_func = V4L2_XFER_FUNC_709;
}

/* Fill a frame with limited-range black (Y 16, Cb/Cr 128). */
static void gc550_fill_black(u8 *p, size_t len)
{
	static const u8 black[4] = { 0x10, 0x80, 0x10, 0x80 };
	size_t i;

	for (i = 0; i < len; i++)
		p[i] = black[i & 3];
}

/* ---- parser side (called from the rx work item under parse_lock) ---- */

u8 *gc550_video_frame_buffer(struct gc550_dev *dev, size_t *capacity)
{
	struct gc550_buffer *buf;
	unsigned long flags;

	lockdep_assert_held(&dev->parse_lock);
	if (!dev->video_on)
		return NULL;
	spin_lock_irqsave(&dev->buf_lock, flags);
	buf = list_first_entry_or_null(&dev->bufs, struct gc550_buffer, list);
	if (buf)
		list_del(&buf->list);
	spin_unlock_irqrestore(&dev->buf_lock, flags);
	if (!buf)
		return NULL;
	dev->cur = buf;
	*capacity = vb2_plane_size(&buf->vb.vb2_buf, 0);
	return vb2_plane_vaddr(&buf->vb.vb2_buf, 0);
}

void gc550_video_frame_done(struct gc550_dev *dev, size_t bytes)
{
	struct gc550_buffer *buf = dev->cur;
	u8 *p;

	lockdep_assert_held(&dev->parse_lock);
	dev->cur = NULL;
	if (!buf)
		return;
	p = vb2_plane_vaddr(&buf->vb.vb2_buf, 0);
	if (gc550_input_protected(dev))
		gc550_fill_black(p, GC550_FRAME_BYTES);
	else if (bytes < GC550_FRAME_BYTES)
		gc550_fill_black(p + bytes, GC550_FRAME_BYTES - bytes);
	vb2_set_plane_payload(&buf->vb.vb2_buf, 0, GC550_FRAME_BYTES);
	buf->vb.vb2_buf.timestamp = ktime_get_ns();
	buf->vb.sequence = dev->sequence++;
	buf->vb.field = V4L2_FIELD_NONE;
	vb2_buffer_done(&buf->vb.vb2_buf, VB2_BUF_STATE_DONE);
}

/* Take back the buffer the parser holds (STREAMOFF). */
void gc550_video_release_buffer(struct gc550_dev *dev)
{
	lockdep_assert_held(&dev->parse_lock);
	if (dev->cur)
		vb2_buffer_done(&dev->cur->vb.vb2_buf, VB2_BUF_STATE_ERROR);
	dev->cur = NULL;
	gc550_parser_release_buffer(&dev->parser);
}

/* ---- vb2 ---- */

static int gc550_queue_setup(struct vb2_queue *q, unsigned int *nbuffers,
			     unsigned int *nplanes, unsigned int sizes[],
			     struct device *alloc_devs[])
{
	if (*nplanes)
		return sizes[0] < GC550_FRAME_BYTES ? -EINVAL : 0;
	*nplanes = 1;
	/* room for a span at the parser's size tolerance */
	sizes[0] = GC550_FRAME_BUFFER_BYTES;
	return 0;
}

static int gc550_buf_prepare(struct vb2_buffer *vb)
{
	if (vb2_plane_size(vb, 0) < GC550_FRAME_BYTES)
		return -EINVAL;
	return 0;
}

static void gc550_buf_queue(struct vb2_buffer *vb)
{
	struct gc550_dev *dev = vb2_get_drv_priv(vb->vb2_queue);
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);
	struct gc550_buffer *buf = container_of(vbuf, struct gc550_buffer, vb);
	unsigned long flags;

	spin_lock_irqsave(&dev->buf_lock, flags);
	list_add_tail(&buf->list, &dev->bufs);
	spin_unlock_irqrestore(&dev->buf_lock, flags);
}

static void gc550_return_all(struct gc550_dev *dev, enum vb2_buffer_state state)
{
	struct gc550_buffer *buf, *tmp;
	unsigned long flags;

	spin_lock_irqsave(&dev->buf_lock, flags);
	list_for_each_entry_safe(buf, tmp, &dev->bufs, list) {
		list_del(&buf->list);
		vb2_buffer_done(&buf->vb.vb2_buf, state);
	}
	spin_unlock_irqrestore(&dev->buf_lock, flags);
}

static int gc550_start_streaming(struct vb2_queue *q, unsigned int count)
{
	struct gc550_dev *dev = vb2_get_drv_priv(q);
	int ret;

	dev->sequence = 0;
	mutex_lock(&dev->parse_lock);
	dev->video_on = true;
	mutex_unlock(&dev->parse_lock);

	ret = gc550_stream_get(dev, GC550_USER_VIDEO);
	if (ret) {
		mutex_lock(&dev->parse_lock);
		dev->video_on = false;
		mutex_unlock(&dev->parse_lock);
		gc550_return_all(dev, VB2_BUF_STATE_QUEUED);
	}
	return ret;
}

static void gc550_stop_streaming(struct vb2_queue *q)
{
	struct gc550_dev *dev = vb2_get_drv_priv(q);

	mutex_lock(&dev->parse_lock);
	dev->video_on = false;
	gc550_video_release_buffer(dev);
	mutex_unlock(&dev->parse_lock);

	gc550_stream_put(dev, GC550_USER_VIDEO);
	gc550_return_all(dev, VB2_BUF_STATE_ERROR);
}

static const struct vb2_ops gc550_vb2_ops = {
	.queue_setup = gc550_queue_setup,
	.buf_prepare = gc550_buf_prepare,
	.buf_queue = gc550_buf_queue,
	.start_streaming = gc550_start_streaming,
	.stop_streaming = gc550_stop_streaming,
};

/* ---- ioctls ---- */

static int gc550_querycap(struct file *file, void *priv, struct v4l2_capability *cap)
{
	struct gc550_dev *dev = video_drvdata(file);

	strscpy(cap->driver, GC550_DRIVER_NAME, sizeof(cap->driver));
	strscpy(cap->card, "AVerMedia Live Gamer Extreme GC550", sizeof(cap->card));
	usb_make_path(dev->udev, cap->bus_info, sizeof(cap->bus_info));
	return 0;
}

static int gc550_enum_fmt(struct file *file, void *priv, struct v4l2_fmtdesc *f)
{
	if (f->index)
		return -EINVAL;
	f->pixelformat = V4L2_PIX_FMT_YUYV;
	return 0;
}

static int gc550_fmt(struct file *file, void *priv, struct v4l2_format *f)
{
	/* G/S/TRY_FMT all get the one fixed format */
	gc550_fill_pix(&f->fmt.pix);
	return 0;
}

static int gc550_enum_framesizes(struct file *file, void *priv,
				 struct v4l2_frmsizeenum *fs)
{
	if (fs->index || fs->pixel_format != V4L2_PIX_FMT_YUYV)
		return -EINVAL;
	fs->type = V4L2_FRMSIZE_TYPE_DISCRETE;
	fs->discrete.width = GC550_FRAME_WIDTH;
	fs->discrete.height = GC550_FRAME_HEIGHT;
	return 0;
}

static int gc550_enum_frameintervals(struct file *file, void *priv,
				     struct v4l2_frmivalenum *fi)
{
	if (fi->index || fi->pixel_format != V4L2_PIX_FMT_YUYV ||
	    fi->width != GC550_FRAME_WIDTH || fi->height != GC550_FRAME_HEIGHT)
		return -EINVAL;
	fi->type = V4L2_FRMIVAL_TYPE_DISCRETE;
	fi->discrete.numerator = 1;
	fi->discrete.denominator = GC550_FPS;
	return 0;
}

static int gc550_parm(struct file *file, void *priv, struct v4l2_streamparm *a)
{
	if (a->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;
	memset(&a->parm.capture, 0, sizeof(a->parm.capture));
	a->parm.capture.capability = V4L2_CAP_TIMEPERFRAME;
	a->parm.capture.timeperframe.numerator = 1;
	a->parm.capture.timeperframe.denominator = GC550_FPS;
	a->parm.capture.readbuffers = 2;
	return 0;
}

static int gc550_enum_input(struct file *file, void *priv, struct v4l2_input *inp)
{
	struct gc550_dev *dev = video_drvdata(file);

	if (inp->index)
		return -EINVAL;
	strscpy(inp->name, "HDMI", sizeof(inp->name));
	inp->type = V4L2_INPUT_TYPE_CAMERA;
	inp->capabilities = 0;

	mutex_lock(&dev->status_lock);
	if (dev->signal_valid) {
		if (!dev->signal.cable)
			inp->status = V4L2_IN_ST_NO_POWER | V4L2_IN_ST_NO_SIGNAL;
		else if (dev->signal.hdcp)
			inp->status = V4L2_IN_ST_NO_ACCESS;
		else if (!gc550_signal_present(&dev->signal))
			inp->status = V4L2_IN_ST_NO_SIGNAL;
	}
	mutex_unlock(&dev->status_lock);
	return 0;
}

static int gc550_g_input(struct file *file, void *priv, unsigned int *i)
{
	*i = 0;
	return 0;
}

static int gc550_s_input(struct file *file, void *priv, unsigned int i)
{
	return i ? -EINVAL : 0;
}

static int gc550_log_status(struct file *file, void *priv)
{
	struct gc550_dev *dev = video_drvdata(file);
	struct gc550_parser_stats st;

	mutex_lock(&dev->parse_lock);
	st = dev->parser.stats;
	mutex_unlock(&dev->parse_lock);
	v4l2_info(&dev->v4l2_dev,
		  "frames %llu, dropped %llu, bad spans %u, desyncs %u, audio packets %llu\n",
		  st.frames, st.frames_dropped, st.bad_spans, st.desyncs, st.audio_packets);
	return 0;
}

static int gc550_subscribe_event(struct v4l2_fh *fh, const struct v4l2_event_subscription *sub)
{
	if (sub->type == V4L2_EVENT_SOURCE_CHANGE)
		return v4l2_src_change_event_subscribe(fh, sub);
	return -EINVAL;
}

static const struct v4l2_ioctl_ops gc550_ioctl_ops = {
	.vidioc_querycap = gc550_querycap,
	.vidioc_enum_fmt_vid_cap = gc550_enum_fmt,
	.vidioc_g_fmt_vid_cap = gc550_fmt,
	.vidioc_s_fmt_vid_cap = gc550_fmt,
	.vidioc_try_fmt_vid_cap = gc550_fmt,
	.vidioc_enum_framesizes = gc550_enum_framesizes,
	.vidioc_enum_frameintervals = gc550_enum_frameintervals,
	.vidioc_g_parm = gc550_parm,
	.vidioc_s_parm = gc550_parm,
	.vidioc_enum_input = gc550_enum_input,
	.vidioc_g_input = gc550_g_input,
	.vidioc_s_input = gc550_s_input,
	.vidioc_log_status = gc550_log_status,
	.vidioc_subscribe_event = gc550_subscribe_event,
	.vidioc_unsubscribe_event = v4l2_event_unsubscribe,
	.vidioc_reqbufs = vb2_ioctl_reqbufs,
	.vidioc_create_bufs = vb2_ioctl_create_bufs,
	.vidioc_prepare_buf = vb2_ioctl_prepare_buf,
	.vidioc_querybuf = vb2_ioctl_querybuf,
	.vidioc_qbuf = vb2_ioctl_qbuf,
	.vidioc_dqbuf = vb2_ioctl_dqbuf,
	.vidioc_expbuf = vb2_ioctl_expbuf,
	.vidioc_streamon = vb2_ioctl_streamon,
	.vidioc_streamoff = vb2_ioctl_streamoff,
};

static const struct v4l2_file_operations gc550_fops = {
	.owner = THIS_MODULE,
	.open = v4l2_fh_open,
	.release = vb2_fop_release,
	.read = vb2_fop_read,
	.poll = vb2_fop_poll,
	.mmap = vb2_fop_mmap,
	.unlocked_ioctl = video_ioctl2,
};

void gc550_v4l2_source_change(struct gc550_dev *dev)
{
	static const struct v4l2_event ev = {
		.type = V4L2_EVENT_SOURCE_CHANGE,
		.u.src_change.changes = V4L2_EVENT_SRC_CH_RESOLUTION,
	};

	if (dev->v4l2_registered)
		v4l2_event_queue(&dev->vdev, &ev);
}

static void gc550_v4l2_release(struct v4l2_device *v4l2_dev)
{
	struct gc550_dev *dev = container_of(v4l2_dev, struct gc550_dev, v4l2_dev);

	v4l2_device_unregister(&dev->v4l2_dev);
	gc550_dev_put(dev);
}

int gc550_v4l2_register(struct gc550_dev *dev)
{
	struct vb2_queue *q = &dev->queue;
	struct video_device *vdev = &dev->vdev;
	int ret;

	mutex_init(&dev->vb_lock);
	spin_lock_init(&dev->buf_lock);
	INIT_LIST_HEAD(&dev->bufs);

	ret = v4l2_device_register(&dev->intf->dev, &dev->v4l2_dev);
	if (ret)
		return ret;
	dev->v4l2_dev.release = gc550_v4l2_release;
	gc550_dev_get(dev);	/* dropped in gc550_v4l2_release() */

	q->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	q->io_modes = VB2_MMAP | VB2_READ | VB2_DMABUF;
	q->drv_priv = dev;
	q->buf_struct_size = sizeof(struct gc550_buffer);
	q->ops = &gc550_vb2_ops;
	q->mem_ops = &vb2_vmalloc_memops;
	q->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
	q->min_queued_buffers = 2;
	q->lock = &dev->vb_lock;
	q->dev = &dev->intf->dev;
	ret = vb2_queue_init(q);
	if (ret)
		goto fail;

	strscpy(vdev->name, "GC550", sizeof(vdev->name));
	vdev->v4l2_dev = &dev->v4l2_dev;
	vdev->fops = &gc550_fops;
	vdev->ioctl_ops = &gc550_ioctl_ops;
	vdev->release = video_device_release_empty;
	vdev->lock = &dev->vb_lock;
	vdev->queue = q;
	vdev->device_caps = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING |
			    V4L2_CAP_READWRITE;
	video_set_drvdata(vdev, dev);

	ret = video_register_device(vdev, VFL_TYPE_VIDEO, -1);
	if (ret)
		goto fail;
	dev->v4l2_registered = true;
	dev_info(&dev->intf->dev, "registered %s\n", video_device_node_name(vdev));
	return 0;
fail:
	v4l2_device_put(&dev->v4l2_dev);
	return ret;
}

void gc550_v4l2_unregister(struct gc550_dev *dev)
{
	if (!dev->v4l2_registered)
		return;
	dev->v4l2_registered = false;
	v4l2_device_disconnect(&dev->v4l2_dev);
	vb2_video_unregister_device(&dev->vdev);
	v4l2_device_put(&dev->v4l2_dev);
}
