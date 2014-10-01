#include "klgd_ff_plugin_p.h"
#include <linux/slab.h>

static bool ffpl_has_gain(const struct ff_effect *eff);
static bool ffpl_needs_replacing(const struct ff_effect *ac_eff, const struct ff_effect *la_eff);

static int ffpl_erase_effect(struct klgd_plugin_private *priv, struct klgd_command_stream *s, struct ffpl_effect *eff)
{
	if (eff->uploaded_to_device) {
		struct input_dev *dev = priv->dev;
		union ffpl_control_data data;
		int ret;

		data.effects.cur = &eff->active;
		data.effects.old = NULL;
		ret = priv->control(dev, s, FFPL_UPL_TO_EMP, data);
		if (ret)
			return ret;
	}

	eff->state = FFPL_EMPTY;
	eff->uploaded_to_device = false;
	return 0;
}

static int ffpl_replace_effect(struct klgd_plugin_private *priv, struct klgd_command_stream *s, struct ffpl_effect *eff,
			       const enum ffpl_control_command cmd)
{
	struct input_dev *dev = priv->dev;
	union ffpl_control_data data;
	int ret;

	data.effects.cur = &eff->latest;
	data.effects.old = &eff->active;
	ret = priv->control(dev, s, cmd, data);
	if (!ret) {
		eff->active = eff->latest;
		eff->state = (cmd == FFPL_OWR_TO_UPL) ? FFPL_UPLOADED : FFPL_STARTED;
		eff->replace = false;
		eff->change = FFPL_DONT_TOUCH;
		return 0;
	}
	return ret;
}

static int ffpl_start_effect(struct klgd_plugin_private *priv, struct klgd_command_stream *s, struct ffpl_effect *eff)
{
	struct input_dev *dev = priv->dev;
	union ffpl_control_data data;
	int ret;
	enum ffpl_control_command cmd;

	data.effects.old = NULL;
	if (priv->upload_when_started && eff->state == FFPL_UPLOADED) {
		data.effects.cur = &eff->active;
		if (eff->uploaded_to_device)
			cmd = FFPL_UPL_TO_SRT;
		else
			cmd = FFPL_EMP_TO_SRT;

		ret = priv->control(dev, s, cmd, data);
		if (ret)
			return ret;
	} else {
		/* This can happen only if device supports "upload and start" */
		if (eff->state == FFPL_EMPTY) {
			data.effects.cur = &eff->latest;
			cmd = FFPL_EMP_TO_SRT;
		} else {
			data.effects.cur = &eff->active;
			cmd = FFPL_UPL_TO_SRT;
		}

		ret = priv->control(dev, s, cmd, data);
		if (ret)
			return ret;
		if (cmd == FFPL_EMP_TO_SRT)
			eff->active = eff->latest;
	}

	eff->uploaded_to_device = true; /* Needed of devices that support "upload and start" but don't use "upload when started" */
	eff->state = FFPL_STARTED;
	return 0;
}

static int ffpl_stop_effect(struct klgd_plugin_private *priv, struct klgd_command_stream *s, struct ffpl_effect *eff)
{
	struct input_dev *dev = priv->dev;
	union ffpl_control_data data;
	int ret;
	enum ffpl_control_command cmd;

	data.effects.cur = &eff->active;
	data.effects.old = NULL;
	if (priv->erase_when_stopped || (priv->has_srt_to_emp && eff->change == FFPL_TO_ERASE))
		cmd = FFPL_SRT_TO_EMP;
	else
		cmd = FFPL_SRT_TO_UPL;

	ret = priv->control(dev, s, cmd, data);
	if (ret)
		return ret;
	if (cmd == FFPL_SRT_TO_EMP)
		eff->uploaded_to_device = false;
	eff->state = FFPL_UPLOADED;
	return 0;
}

static int ffpl_update_effect(struct klgd_plugin_private *priv, struct klgd_command_stream *s, struct ffpl_effect *eff)
{
	struct input_dev *dev = priv->dev;
	union ffpl_control_data data;
	int ret;

	if (!eff->uploaded_to_device)
		return ffpl_start_effect(priv, s, eff);

	data.effects.cur = &eff->latest;
	data.effects.old = NULL;
	ret = priv->control(dev, s, FFPL_SRT_TO_UDT, data);
	if (ret)
		return ret;
	eff->active = eff->latest;
	return 0;
}

static int ffpl_upload_effect(struct klgd_plugin_private *priv, struct klgd_command_stream *s, struct ffpl_effect *eff)
{
	if (!priv->upload_when_started) {
		struct input_dev *dev = priv->dev;
		union ffpl_control_data data;
		int ret;

		data.effects.cur = &eff->latest;
		data.effects.old = NULL;
		ret = priv->control(dev, s, FFPL_EMP_TO_UPL, data);
		if (ret)
			return ret;
		eff->uploaded_to_device = true;
	}

	eff->state = FFPL_UPLOADED;
	eff->active = eff->latest;
	return 0;
}

/* Destroy request - input device is being destroyed */
static void ffpl_destroy_rq(struct ff_device *ff)
{	
	struct klgd_plugin *self = ff->private;
	struct klgd_plugin_private *priv = self->private;

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

	/* Copy the new effect to the "latest" slot */
	eff->latest = *effect;

	if (eff->state != FFPL_EMPTY) {
		if (ffpl_needs_replacing(&eff->active, &eff->latest))
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

/*FIXME: Rewrite this! */
static void ffpl_set_gain_rq(struct input_dev *dev, u16 gain)
{
	struct klgd_plugin *self = dev->ff->private;
	struct klgd_plugin_private *priv = self->private;
	size_t idx;

	klgd_lock_plugins(self->plugins_lock);

	printk(KERN_DEBUG "KLGDFF: Gain set, %u\n", gain);
	for (idx = 0; idx < priv->effect_count; idx++) {
		struct ffpl_effect *eff = &priv->effects[idx];

		if (ffpl_has_gain(&eff->active))
			eff->change = FFPL_TO_UPDATE;
	}
	/* priv->gain = gain;*/

	klgd_unlock_plugins_sched(self->plugins_lock);
}

static void ffpl_deinit(struct klgd_plugin *self)
{
	printk(KERN_DEBUG "KLGDFF: Deinit complete\n");
}

static struct klgd_command_stream * ffpl_get_commands(struct klgd_plugin *self, const unsigned long now)
{
	struct klgd_plugin_private *priv = self->private;
	struct klgd_command_stream *s;
	size_t idx;

	s = klgd_alloc_stream();
	if (!s)
		return NULL; /* TODO: Error handling */
  
	for (idx = 0; idx < priv->effect_count; idx++) {
		struct ffpl_effect *eff = &priv->effects[idx];
		int ret = 0;

		/* Latest effect is of different type than currently active effect,
		 * remove it from the device and upload the latest one */
		if (eff->replace) {
			switch (eff->change) {
			case FFPL_TO_ERASE:
				switch (eff->state) {
				case FFPL_STARTED:
					ret = ffpl_stop_effect(priv, s, eff);
					if (ret)
						break;
				case FFPL_UPLOADED:
					ret = ffpl_erase_effect(priv, s, eff);
					/* TODO: Handle error */
				default:
					/* Nothing to do - the effect that is replacing the old effect is about to be erased anyway 
					 * State of the effect to be replaced should also never be EMPTY */
					break;
				}
				break;
			case FFPL_TO_UPLOAD:
			case FFPL_TO_STOP: /* There is no difference between stopping or uploading an effect when we are replacing it */
				switch (eff->state) {
				case FFPL_STARTED:
					/* Overwrite the currently active effect and set it to UPLOADED state */
					if (priv->has_owr_to_upl) {
						ret = ffpl_replace_effect(priv, s, eff, FFPL_OWR_TO_UPL);
						if (ret)
							break;
						continue;
					}
					ret = ffpl_stop_effect(priv, s, eff);
					if (ret)
						break;
				case FFPL_UPLOADED:
					ret = ffpl_erase_effect(priv, s, eff);
					if (ret)
						break;
				case FFPL_EMPTY: /* State cannot actually be FFPL_EMPTY becuase only uploaded or started effects have to be replaced like this */
					ret = ffpl_upload_effect(priv, s, eff);
					break;
				}
				break;
			case FFPL_TO_START:
			case FFPL_TO_UPDATE: /* There is no difference between staring or updating an effect when we are replacing it */
				switch (eff->state) {
				case FFPL_STARTED:
					if (priv->has_owr_to_srt) {
						ret = ffpl_replace_effect(priv, s, eff, FFPL_OWR_TO_SRT);
						if (ret)
							break;
						continue;
					}
					ret = ffpl_stop_effect(priv, s, eff);
					if (ret)
						break;
				case FFPL_UPLOADED:
					ret = ffpl_erase_effect(priv, s, eff);
					if (ret)
						break;
				case FFPL_EMPTY: /* State cannot actually be FFPL_EMPTY - same as above applies */
					ret = ffpl_upload_effect(priv, s, eff);
					if (ret)
						break;
					ret = ffpl_start_effect(priv, s, eff);
					break;
				}
			case FFPL_DONT_TOUCH:
				printk(KERN_WARNING "Got FFPL_DONT_TOUCH change for effect that should be replaced - this should not happen!\n");
				break;
			default:
				printk(KERN_WARNING "Unhandled state change while replacing effect\n");
				break;
			}
			if (ret)
				printk(KERN_WARNING "Error %d while replacing effect %lu\n", ret, idx);
			else {
				eff->replace = false;
				eff->state = FFPL_DONT_TOUCH;
				continue;
			}
		}

		switch (eff->change) {
		case FFPL_TO_UPLOAD:
			switch (eff->state) {
			case FFPL_EMPTY:
				ret = ffpl_upload_effect(priv, s, eff);
				break;
			case FFPL_STARTED:
				ret = ffpl_stop_effect(priv, s, eff);
				break;
			default:
				break;
			}
			break;
		case FFPL_TO_START:
			switch (eff->state) {
			case FFPL_EMPTY:
				if (priv->has_emp_to_srt)
					ret = ffpl_start_effect(priv, s, eff);
				else
					ret = ffpl_upload_effect(priv, s, eff);
				if (ret)
					break;
			case FFPL_UPLOADED:
				ret = ffpl_start_effect(priv, s, eff);
				break;
			default:
				break;
			}
			break;
		case FFPL_TO_STOP:
			switch (eff->state) {
			case FFPL_STARTED:
				ret = ffpl_stop_effect(priv, s, eff);
				break;
			case FFPL_EMPTY:
				ret = ffpl_upload_effect(priv, s, eff);
				break;
			default:
				break;
			}
			break;
		case FFPL_TO_ERASE:
			switch (eff->state) {
			case FFPL_UPLOADED:
				ret = ffpl_erase_effect(priv, s, eff);
				break;
			case FFPL_STARTED:
				ret = ffpl_stop_effect(priv, s, eff);
				if (ret)
					break;
				ret = ffpl_erase_effect(priv, s, eff);
				break;
			default:
				break;
			}
			break;
		case FFPL_TO_UPDATE:
			ret = ffpl_update_effect(priv, s, eff);
			break;
		case FFPL_DONT_TOUCH:
			continue;
		default:
			pr_debug("Unhandled state\n");
		}

		/* TODO: Handle errors */
		if (ret)
			printk(KERN_WARNING "Error %d while processing effect %lu\n", ret, idx);
		else
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
		     const unsigned long flags,
		     int (*control)(struct input_dev *dev, struct klgd_command_stream *s, const enum ffpl_control_command cmd, const union ffpl_control_data data))
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
	for (idx = 0; idx < effect_count; idx++) {
		priv->effects[idx].replace = false;
		priv->effects[idx].uploaded_to_device = false;
		priv->effects[idx].state = FFPL_EMPTY;
		priv->effects[idx].change = FFPL_DONT_TOUCH;
	}

	self->deinit = ffpl_deinit;
	self->get_commands = ffpl_get_commands;
	self->get_update_time = ffpl_get_update_time;
	self->init = ffpl_init;
	priv->supported_effects = supported_effects;
	priv->effect_count = effect_count;
	priv->dev = dev;
	priv->control = control;

	self->private = priv;
	*plugin = self;

	if (FFPL_HAS_EMP_TO_SRT & flags) {
		priv->has_emp_to_srt = true;
		printk("KLGDFF: Using HAS EMP_TO_SRT\n");
	}
	if (FFPL_HAS_SRT_TO_EMP & flags) {
		priv->has_srt_to_emp = true;
		printk("KLGDFF: Using HAS SRT_TO_EMP\n");
	}
	if (FFPL_UPLOAD_WHEN_STARTED & flags) {
		priv->has_emp_to_srt = true;
		priv->upload_when_started = true;
		printk("KLGDFF: Using UPLOAD WHEN STARTED\n");
	}
	if (FFPL_ERASE_WHEN_STOPPED & flags) {
		priv->has_srt_to_emp = true;
		priv->erase_when_stopped = true;
		printk("KLGDFF: Using ERASE WHEN STOPPED\n");
	}
	if (FFPL_REPLACE_UPLOADED & flags) {
		priv->has_owr_to_upl = true;
		printk("KLGDFF: Using REPLACE UPLOADED\n");
	}
	if (FFPL_REPLACE_STARTED & flags) {
		priv->has_owr_to_srt = true;
		printk("KLGDFF: Using REPLACE STARTED\n");
	}

	set_bit(FF_GAIN, dev->ffbit);
	for (idx = 0; idx <= (FF_EFFECT_MAX - FF_EFFECT_MIN); idx++) {
		if (test_bit(idx, &priv->supported_effects)) {
			printk(KERN_NOTICE "KLGDFF: Has bit %d, effect type %d\n", idx, FF_EFFECT_MIN + idx);
			input_set_capability(dev, EV_FF, idx + FF_EFFECT_MIN);
		}
	}
	return 0;

err_out2:
	kfree(priv);
err_out1:
	kfree(self);
	return ret;
}

static bool ffpl_needs_replacing(const struct ff_effect *ac_eff, const struct ff_effect *la_eff)
{
	if (ac_eff->type != la_eff->type) {
		printk(KERN_NOTICE "KLGDFF - Effects are of different type - replacing (%d x %d)\n", ac_eff->type, la_eff->type);
		return true;
	}

	if (ac_eff->type == FF_PERIODIC) {
		if (ac_eff->u.periodic.waveform != la_eff->u.periodic.waveform) {
			printk(KERN_NOTICE "KLGDFF - Effects have different waveforms - replacing\n");
			return true;
		}
	}

	printk(KERN_NOTICE "KLGDFF - Effect does not have to be replaced, updating\n");
	return false;
}
