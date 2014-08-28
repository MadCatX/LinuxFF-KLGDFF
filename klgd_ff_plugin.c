#include "klgd_ff_plugin.h"
#include "klgd_ff_plugin_p.h"
#include <linux/slab.h>

static bool ffpl_has_gain(const struct ff_effect *eff);
static bool ffpl_replace_effect(const struct ff_effect *ac_eff, const struct ff_effect *la_eff);

/* Destroy request - input device is being destroyed */
static void ffpl_destroy_rq(struct ff_device *ff)
{	
	struct klgd_plugin *self = ff->private;
	struct klgd_plugin_private *priv = self->private;
	size_t idx;

	for (idx = 0; idx < priv->effect_count; idx++) {
		struct ffpl_effect *eff = &priv->effects[idx];

		kfree(eff->active);
		if (eff->active != eff->latest)
			kfree(eff->latest);
	}
	kfree(priv->effects);
	kfree(priv);
}

/* Erase request coming from userspace */
static int ffpl_erase_rq(struct input_dev *dev, int effect_id)
{
	struct klgd_plugin *self = dev->ff->private;
	struct klgd_plugin_private *priv = self->private;
	struct ffpl_effect *eff = &priv->effects[effect_id];

	klgd_lock_plugins(self->plugins_lock);
	eff->change = FFPL_TO_ERASE;
	klgd_unlock_plugins_sched(self->plugins_lock);

	return 0;
}

/* Playback request coming from userspace */
static int ffpl_playback_rq(struct input_dev *dev, int effect_id, int value)
{
	struct klgd_plugin *self = dev->ff->private;
	struct klgd_plugin_private *priv = self->private;
	struct ffpl_effect *eff = &priv->effects[effect_id];

	klgd_lock_plugins(self->plugins_lock);

	if (value)
		eff->change = FFPL_TO_START;
	else
		eff->change = FFPL_TO_STOP;

	klgd_unlock_plugins_sched(self->plugins_lock);

	return 0;
}

/* Upload request coming from userspace */
static int ffpl_upload_rq(struct input_dev *dev, struct ff_effect *effect, struct ff_effect *old)
{
	struct klgd_plugin *self = dev->ff->private;
	struct klgd_plugin_private *priv = self->private;
	struct ffpl_effect *eff = &priv->effects[effect->id];

	klgd_lock_plugins(self->plugins_lock);
	spin_lock_irq(&dev->event_lock);


	/* Latest effect is not in use, free it */
	if (eff->latest != eff->active)
		kfree(eff->latest);
	/* Copy the new effect to the "latest" slot */
	eff->latest = kmemdup(effect, sizeof(struct ff_effect), GFP_KERNEL);

	if (eff->state != FFPL_EMPTY) {
		if (ffpl_replace_effect(eff->active, eff->latest))
			eff->replace = true;
		else {
			eff->replace = false;
			eff->change = FFPL_TO_UPDATE;
		}
	} else
		eff->change = FFPL_TO_UPLOAD;

	spin_unlock_irq(&dev->event_lock);
	klgd_unlock_plugins_sched(self->plugins_lock);

	return 0;
}


static void ffpl_set_gain_rq(struct input_dev *dev, u16 gain)
{
	struct klgd_plugin *self = dev->ff->private;
	struct klgd_plugin_private *priv = self->private;
	size_t idx;

	klgd_lock_plugins(self->plugins_lock);

	printk(KERN_DEBUG "KLGDFF: Gain set, %u\n", gain);
	for (idx = 0; idx < priv->effect_count; idx++) {
		struct ffpl_effect *eff = &priv->effects[idx];

		if (ffpl_has_gain(eff->active))
			eff->change = FFPL_TO_UPDATE;
	}
	priv->gain = gain;

	klgd_unlock_plugins_sched(self->plugins_lock);
}

static void ffpl_deinit(struct klgd_plugin *self)
{
	printk(KERN_DEBUG "KLGDFF: Deinit complete\n");
}

static struct klgd_command_stream * ffpl_get_commands(struct klgd_plugin *self, const unsigned long now)
{
	struct klgd_plugin_private *priv = self->private;
	struct input_dev *const dev = priv->dev;
	struct klgd_command_stream *s;
	size_t idx;

	s = klgd_alloc_stream();
	if (!s)
		return NULL; /* TODO: Error handling */
  
	for (idx = 0; idx < priv->effect_count; idx++) {
		struct ffpl_effect *eff = &priv->effects[idx];

		/* Effect has not been touched since the last update, skip it */
		if (eff->change == FFPL_DONT_TOUCH)
			continue;

		/* Latest effect is of different type than currently active effect,
		 * remove it from the device and upload the latest one */
		if (eff->replace) {
			switch (eff->state) {
			case FFPL_STARTED:
				klgd_append_cmd(s, priv->stop_effect(dev, eff->active, idx));
			default:
				klgd_append_cmd(s, priv->erase_effect(dev, eff->active, idx));
				kfree(eff->active);
				eff->active = NULL;
				break;
			}
			eff->replace = false;

			/* The new effect is to be erased anyway */
			if (eff->change == FFPL_TO_ERASE) {
				eff->change = FFPL_DONT_TOUCH;
				continue;
			}
		}

		/* Figure out the state change since the last update and build
		 * command stream accordingly */
		switch (eff->state) {
		case FFPL_EMPTY:
			switch (eff->change) {
			case FFPL_TO_UPLOAD:
			case FFPL_TO_STOP:
				klgd_append_cmd(s, priv->upload_effect(dev, eff->latest, idx));
				eff->active = eff->latest;
				eff->state = FFPL_UPLOADED;
				break;
			case FFPL_TO_START:
				klgd_append_cmd(s, priv->upload_effect(dev, eff->latest, idx));
				eff->active = eff->latest;
				klgd_append_cmd(s, priv->start_effect(dev, eff->active, idx));
				eff->state = FFPL_STARTED;
				break;
			default:
				break;
			}
			break;
		case FFPL_UPLOADED:
			switch (eff->change) {
			case FFPL_TO_START:
				klgd_append_cmd(s, priv->start_effect(dev, eff->active, idx));
				eff->state = FFPL_STARTED;
				break;
			case FFPL_TO_ERASE:
				klgd_append_cmd(s, priv->erase_effect(dev, eff->active, idx));
				kfree(eff->active);
				if (eff->active != eff->latest)
					kfree(eff->latest);
				eff->latest = NULL;
				eff->active = NULL;
				eff->state = FFPL_EMPTY;
				break;
			case FFPL_TO_UPDATE:
				klgd_append_cmd(s, priv->upload_effect(dev, eff->latest, idx));
				eff->active = eff->latest;
				break;
			default:
				break;
			}
			break;
		case FFPL_STARTED:
			switch (eff->change) {
			case FFPL_TO_STOP:
				klgd_append_cmd(s, priv->stop_effect(dev, eff->active, idx));
				eff->state = FFPL_UPLOADED;
				break;
			case FFPL_TO_ERASE:
				klgd_append_cmd(s, priv->stop_effect(dev, eff->active, idx));
				klgd_append_cmd(s, priv->erase_effect(dev, eff->active, idx));
				kfree(eff->active);
				if (eff->active != eff->latest)
					kfree(eff->latest);
				eff->latest = NULL;
				eff->active = NULL;
				eff->state = FFPL_EMPTY;
				break;
			case FFPL_TO_UPDATE:
				klgd_append_cmd(s, priv->upload_effect(dev, eff->latest, idx));
				eff->active = eff->latest;
			default:
				break;
			}
			break;
		}
		eff->change = FFPL_DONT_TOUCH;
	}

	return s;
}

static bool ffpl_get_update_time(struct klgd_plugin *self, const unsigned long now, unsigned long *t)
{
	struct klgd_plugin_private *priv = self->private;
	size_t idx, events = 0;

	for (idx = 0; idx < priv->effect_count; idx++) {
		struct ffpl_effect *eff = &priv->effects[idx];

		/* Tell KLGD to attend to us as soon as possible if an effect has to change state */
		if (eff->change == FFPL_DONT_TOUCH)
			continue;
		*t = now;
		events++;
	}

	return events ? true : false;
}

static bool ffpl_has_gain(const struct ff_effect *eff)
{
	switch (eff->type) {
	case FF_CONSTANT:
        case FF_PERIODIC:
	case FF_RAMP:
	case FF_RUMBLE:
		return true;
	default:
		return false;
	}
}

static int ffpl_init(struct klgd_plugin *self)
{
	struct klgd_plugin_private *priv = self->private;
	struct input_dev *dev = priv->dev;
	int ret;

	ret = input_ff_create(dev, priv->effect_count);
	if (ret)
		return ret;

	dev->ff->private = self;
	dev->ff->erase = ffpl_erase_rq;
	dev->ff->playback = ffpl_playback_rq;
	dev->ff->upload = ffpl_upload_rq;
	dev->ff->set_gain = ffpl_set_gain_rq;
	dev->ff->destroy = ffpl_destroy_rq;
	printk(KERN_NOTICE "KLGDFF: Init complete\n");

	return 0;
}
		
/* Initialize the plugin */
int ffpl_init_plugin(struct klgd_plugin **plugin, struct input_dev *dev, const size_t effect_count,
		     const unsigned long supported_effects,
		     struct klgd_command * (*upload)(struct input_dev *dev, const struct ff_effect *effect, const int id),
		     struct klgd_command * (*start)(struct input_dev *dev, const struct ff_effect *effect, const int id),
		     struct klgd_command * (*stop)(struct input_dev *dev, const struct ff_effect *effect, const int id),
		     struct klgd_command * (*erase)(struct input_dev *dev, const struct ff_effect *effect, const int id))
{
	struct klgd_plugin *self;
	struct klgd_plugin_private *priv;
	int ret, idx;	

	self = kzalloc(sizeof(struct klgd_plugin), GFP_KERNEL);
	if (!self)
		return -ENOMEM;

	priv = kzalloc(sizeof(struct klgd_plugin_private), GFP_KERNEL);
	if (!priv) {
		ret = -ENOMEM;
		goto err_out1;
	}

	priv->effects = kzalloc(sizeof(struct ffpl_effect) * effect_count, GFP_KERNEL);
	if (!priv->effects) {
		ret = -ENOMEM;
		goto err_out2;
	}

	self->deinit = ffpl_deinit;
	self->get_commands = ffpl_get_commands;
	self->get_update_time = ffpl_get_update_time;
	self->init = ffpl_init;
	priv->supported_effects = supported_effects;
	priv->effect_count = effect_count;
	priv->dev = dev;
	priv->upload_effect = upload;
	priv->start_effect = start;
	priv->stop_effect = stop;
	priv->erase_effect = erase;

	self->private = priv;
	*plugin = self;

	set_bit(FF_GAIN, dev->ffbit);
	for (idx = 0; idx <= (FF_EFFECT_MAX - FF_EFFECT_MIN); idx++) {
		if (test_bit(idx, &priv->supported_effects)) {
			printk(KERN_NOTICE "KLGDFF: Has bit %d, effect type %d\n", idx, FF_EFFECT_MIN + idx);
			input_set_capability(dev, EV_FF, idx + FF_EFFECT_MIN);
		}
	}
	return 0;

err_out2:
	kfree(self->private->effects);
err_out1:
	kfree(self);
	return ret;
}

static bool ffpl_replace_effect(const struct ff_effect *ac_eff, const struct ff_effect *la_eff)
{
	if (ac_eff->type != la_eff->type)
		return true;

	if (ac_eff->type == FF_PERIODIC) {
		if (ac_eff->u.periodic.waveform != la_eff->u.periodic.waveform)
			return true;
	}

	return false;
}
