// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * AVerMedia Live Gamer Extreme (GC550) ALSA capture device: the HDMI audio
 * carried in the stream, 48 kHz S16_LE stereo.
 */
#include <sound/core.h>
#include <sound/initval.h>
#include <sound/pcm.h>

#include "gc550.h"

static int index = SNDRV_DEFAULT_IDX1;
module_param(index, int, 0444);
MODULE_PARM_DESC(index, "ALSA card index for the GC550");

static const struct snd_pcm_hardware gc550_pcm_hw = {
	.info = SNDRV_PCM_INFO_MMAP | SNDRV_PCM_INFO_MMAP_VALID |
		SNDRV_PCM_INFO_INTERLEAVED | SNDRV_PCM_INFO_BLOCK_TRANSFER,
	.formats = SNDRV_PCM_FMTBIT_S16_LE,
	.rates = SNDRV_PCM_RATE_48000,
	.rate_min = GC550_AUDIO_RATE,
	.rate_max = GC550_AUDIO_RATE,
	.channels_min = GC550_AUDIO_CHANNELS,
	.channels_max = GC550_AUDIO_CHANNELS,
	.buffer_bytes_max = 256 * 1024,
	.period_bytes_min = 1024,
	.period_bytes_max = 64 * 1024,
	.periods_min = 2,
	.periods_max = 128,
};

/* ---- parser side (rx work item, under parse_lock) ---- */

void gc550_audio_data(struct gc550_dev *dev, const u8 *data, size_t len)
{
	struct snd_pcm_runtime *rt;
	size_t ring, period, n;
	unsigned long flags;

	spin_lock_irqsave(&dev->audio_lock, flags);
	if (!dev->audio_on || !dev->substream) {
		spin_unlock_irqrestore(&dev->audio_lock, flags);
		return;
	}
	rt = dev->substream->runtime;
	ring = frames_to_bytes(rt, rt->buffer_size);
	period = frames_to_bytes(rt, rt->period_size);
	while (len) {
		n = min(len, ring - dev->audio_pos);
		memcpy(rt->dma_area + dev->audio_pos, data, n);
		dev->audio_pos = (dev->audio_pos + n) % ring;
		dev->audio_period_fill += n;
		data += n;
		len -= n;
	}
	if (dev->audio_period_fill >= period) {
		dev->audio_period_fill %= period;
		dev->audio_elapsed = true;
	}
	spin_unlock_irqrestore(&dev->audio_lock, flags);
}

/* Called by the rx work item outside parse_lock. */
void gc550_audio_flush_periods(struct gc550_dev *dev)
{
	struct snd_pcm_substream *ss = NULL;
	unsigned long flags;

	spin_lock_irqsave(&dev->audio_lock, flags);
	if (dev->audio_elapsed && dev->audio_on)
		ss = dev->substream;
	dev->audio_elapsed = false;
	spin_unlock_irqrestore(&dev->audio_lock, flags);
	if (ss)
		snd_pcm_period_elapsed(ss);
}

/* ---- PCM ops (nonatomic: all may sleep) ---- */

static int gc550_pcm_open(struct snd_pcm_substream *ss)
{
	struct gc550_dev *dev = snd_pcm_substream_chip(ss);
	unsigned long flags;

	ss->runtime->hw = gc550_pcm_hw;
	spin_lock_irqsave(&dev->audio_lock, flags);
	dev->substream = ss;
	spin_unlock_irqrestore(&dev->audio_lock, flags);
	return 0;
}

static int gc550_pcm_close(struct snd_pcm_substream *ss)
{
	struct gc550_dev *dev = snd_pcm_substream_chip(ss);
	unsigned long flags;

	spin_lock_irqsave(&dev->audio_lock, flags);
	dev->audio_on = false;
	dev->substream = NULL;
	spin_unlock_irqrestore(&dev->audio_lock, flags);
	return 0;
}

static int gc550_pcm_hw_params(struct snd_pcm_substream *ss, struct snd_pcm_hw_params *hw)
{
	struct gc550_dev *dev = snd_pcm_substream_chip(ss);
	int ret = 0;

	if (!dev->audio_hw) {
		ret = gc550_stream_get(dev, GC550_USER_AUDIO);
		dev->audio_hw = !ret;
	}
	return ret;
}

static int gc550_pcm_hw_free(struct snd_pcm_substream *ss)
{
	struct gc550_dev *dev = snd_pcm_substream_chip(ss);

	if (dev->audio_hw) {
		gc550_stream_put(dev, GC550_USER_AUDIO);
		dev->audio_hw = false;
	}
	return 0;
}

static int gc550_pcm_prepare(struct snd_pcm_substream *ss)
{
	struct gc550_dev *dev = snd_pcm_substream_chip(ss);
	unsigned long flags;

	spin_lock_irqsave(&dev->audio_lock, flags);
	dev->audio_pos = 0;
	dev->audio_period_fill = 0;
	dev->audio_elapsed = false;
	spin_unlock_irqrestore(&dev->audio_lock, flags);
	return 0;
}

static int gc550_pcm_trigger(struct snd_pcm_substream *ss, int cmd)
{
	struct gc550_dev *dev = snd_pcm_substream_chip(ss);
	unsigned long flags;
	int ret = 0;

	spin_lock_irqsave(&dev->audio_lock, flags);
	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
		dev->audio_on = true;
		break;
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
		dev->audio_on = false;
		break;
	default:
		ret = -EINVAL;
	}
	spin_unlock_irqrestore(&dev->audio_lock, flags);
	return ret;
}

static snd_pcm_uframes_t gc550_pcm_pointer(struct snd_pcm_substream *ss)
{
	struct gc550_dev *dev = snd_pcm_substream_chip(ss);
	unsigned long flags;
	size_t pos;

	spin_lock_irqsave(&dev->audio_lock, flags);
	pos = dev->audio_pos;
	spin_unlock_irqrestore(&dev->audio_lock, flags);
	return bytes_to_frames(ss->runtime, pos);
}

static const struct snd_pcm_ops gc550_pcm_ops = {
	.open = gc550_pcm_open,
	.close = gc550_pcm_close,
	.hw_params = gc550_pcm_hw_params,
	.hw_free = gc550_pcm_hw_free,
	.prepare = gc550_pcm_prepare,
	.trigger = gc550_pcm_trigger,
	.pointer = gc550_pcm_pointer,
};

static void gc550_card_free(struct snd_card *card)
{
	gc550_dev_put(card->private_data);
}

int gc550_alsa_register(struct gc550_dev *dev)
{
	struct snd_card *card;
	struct snd_pcm *pcm;
	int ret;

	spin_lock_init(&dev->audio_lock);
	ret = snd_card_new(&dev->intf->dev, index, "GC550", THIS_MODULE, 0, &card);
	if (ret)
		return ret;
	card->private_data = dev;
	card->private_free = gc550_card_free;
	gc550_dev_get(dev);	/* dropped in gc550_card_free() */

	strscpy(card->driver, GC550_DRIVER_NAME, sizeof(card->driver));
	strscpy(card->shortname, "GC550", sizeof(card->shortname));
	snprintf(card->longname, sizeof(card->longname),
		 "AVerMedia Live Gamer Extreme GC550 at %s", dev_name(&dev->udev->dev));

	ret = snd_pcm_new(card, "GC550 HDMI", 0, 0, 1, &pcm);
	if (ret)
		goto fail;
	pcm->private_data = dev;
	pcm->nonatomic = true;
	strscpy(pcm->name, "GC550 HDMI", sizeof(pcm->name));
	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_CAPTURE, &gc550_pcm_ops);
	snd_pcm_set_managed_buffer_all(pcm, SNDRV_DMA_TYPE_VMALLOC, NULL, 0, 0);

	ret = snd_card_register(card);
	if (ret)
		goto fail;
	dev->card = card;
	dev->snd_registered = true;
	return 0;
fail:
	snd_card_free(card);
	return ret;
}

void gc550_alsa_unregister(struct gc550_dev *dev)
{
	if (!dev->snd_registered)
		return;
	dev->snd_registered = false;
	snd_card_disconnect(dev->card);
	snd_card_free_when_closed(dev->card);
}
