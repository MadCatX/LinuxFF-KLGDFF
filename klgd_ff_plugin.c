#include "klgd_ff_plugin_p.h"
#include <linux/slab.h>
#include <linux/fixp-arith.h>

#define DIR_TO_DEGREES(dir) (360 - ((((dir > 0xc000) ? (u32)dir + 0x4000 - 0xffff : (u32)dir + 0x4000) * 360) / 0xffff))

static int ffpl_handle_state_change(struct klgd_plugin_private *priv, struct klgd_command_stream *s, struct ffpl_effect *eff,
				    const bool ignore_combined);
static bool ffpl_has_gain(const struct ff_effect *eff);
static bool ffpl_needs_replacing(const struct ff_effect *ac_eff, const struct ff_effect *la_eff);

static bool ffpl_is_combinable(const struct ff_effect *eff)
{
	/* TODO: Proper decision of what is a combinable effect */
	return eff->type == FF_CONSTANT;
}

static u16 ffpl_atan_int_octet(const u16 x, const u16 y)
{
    u32 result;

    if (!y)
        return 0x0000;

    /* 3rd order polynomial approximation beween 0 <= y/x <= 1 */
    result = y * 0x02e2 / x;
    result = y * (0x05dc + result) / x;
    result = y * (0x28be - result) / x;

    return result;
}

static u16 ffpl_atan_int_quarter(const u16 x, const u16 y)
{
    if (x == y)
        return 0x2000;
    else if (x > y)
        return ffpl_atan_int_octet(x, y);
    else
        return 0x4000 - ffpl_atan_int_octet(y, x);
}


static void ffpl_x_y_to_lvl_dir(const s32 x, const s32 y, s16 *level, u16 *direction)
{
	u16 angle;
	unsigned long pwr;
	u32 _x = abs(x);
	u32 _y = abs(y);

	/* It is necessary to make sure that neither of the coordinates exceeds 0xffff.
	 */
	if (abs(x) > 0xffff || abs(y) > 0xffff) {
		u32 div;
		u32 divx = _x / 0xfffe;
		u32 divy = _y / 0xfffe;

		div = (divx > divy) ? divx : divy;

		_x /= div;
		_y /= div;

		if (_x > 0xffff || _y > 0xffff) {
			_x >>= 1;
			_y >>= 1;
		}

	}
	angle = (!_x && !_y) ? 0x4000 : ffpl_atan_int_quarter(_x, _y);


	/* 1st quadrant */
	if (x >= 0 && y >= 0)
		*direction = 0xC000 - angle;
	/* 2nd quadrant */
	else if (x < 0 && y >= 0)
		*direction = 0x4000 + angle;
	/* 3rd quadrant */
	else if (x < 0 && y < 0)
		*direction = 0x4000 - angle;
	/* 4th quadrant */
	else if (x > 0 && y < 0)
		*direction = 0xC000 + angle;
	else
		*direction = 0x0000;

	if (abs(x) > 0x7fff || abs(y) > 0x7fff) {
		*level = 0x7fff;
		return;
	}

	pwr = int_sqrt(x * x + y * y);
	*level = (pwr > 0x7fff) ? 0x7fff : pwr;
}

bool ffpl_constant_force_to_x_y(const struct ff_effect *eff, s32 *x, s32 *y)
{
	int degrees;

	if (eff->type != FF_CONSTANT)
		return false;

	degrees = DIR_TO_DEGREES(eff->direction);
	printk(KERN_NOTICE "KLGDFF: DIR_TO_DEGREES > Dir: %u, Deg: %u\n", eff->direction, degrees);
	*x += (eff->u.constant.level * fixp_cos(degrees)) >> FRAC_N;
	*y += (eff->u.constant.level * fixp_sin(degrees)) >> FRAC_N;

	return true;
}

static void ffpl_recalc_combined(struct klgd_plugin_private *priv)
{
	size_t idx;
	struct ff_effect *cb_latest = &priv->combined_effect.latest;
	s32 x = 0;
	s32 y = 0;

	for (idx = 0; idx < priv->effect_count; idx++) {
		const struct ffpl_effect *eff = &priv->effects[idx];

		switch (eff->change) {
		case FFPL_DONT_TOUCH:
			if (eff->state != FFPL_STARTED)
				break;
		case FFPL_TO_START:
		case FFPL_TO_UPDATE:
			ffpl_constant_force_to_x_y(&eff->latest, &x, &y);
			break;
		default:
			break;
		}
	}

	ffpl_x_y_to_lvl_dir(x, y, &cb_latest->u.constant.level, &cb_latest->direction);
	cb_latest->type = FF_CONSTANT;
	printk(KERN_NOTICE "KLGDFF: Resulting combined CF effect > x: %d, y: %d, level: %d, direction: %u\n", x, y, cb_latest->u.constant.level,
	       cb_latest->direction);
}

static int ffpl_handle_combinable_effects(struct klgd_plugin_private *priv, struct klgd_command_stream *s)
{
	size_t idx;
	bool needs_update = false;
	size_t active_effects = 0;

	for (idx = 0; idx < priv->effect_count; idx++) {
		struct ffpl_effect *eff = &priv->effects[idx];

		if (!ffpl_is_combinable(&eff->latest))
			continue;

		switch (eff->change) {
		case FFPL_DONT_TOUCH:
			if (eff->state == FFPL_STARTED)
				active_effects++;
			printk(KERN_NOTICE "KLGDFF: Unchanged combinable effect, total active effects %lu\n", active_effects);
			break;
		case FFPL_TO_START:
			eff->state = FFPL_STARTED;
			eff->active = eff->latest;
		case FFPL_TO_UPDATE:
			active_effects++;
			needs_update = true;
			printk(KERN_NOTICE "KLGDFF: Altered combinable effect, total active effects %lu\n", active_effects);
			break;
		case FFPL_TO_STOP:
			if (eff->state == FFPL_STARTED)
				needs_update = true;
		case FFPL_TO_UPLOAD:
			eff->state = FFPL_UPLOADED;
			printk(KERN_NOTICE "KLGDFF: Combinable effect to upload/stop, marking as uploaded\n");
			break;
		default:
			if (eff->state != FFPL_STARTED)
				break;
			needs_update = true;
			eff->state = FFPL_EMPTY;
			printk(KERN_NOTICE "KLGDFF: Stopped combinable effect, total active effects %lu\n", active_effects);
			break;
		}
	}

	/* Combined effect needs recalculation */
	if (needs_update) {
		if (active_effects) {
			printk(KERN_NOTICE "KLGDFF: Combined effect needs an update, total effects active: %lu\n", active_effects);
			ffpl_recalc_combined(priv);
			if (priv->combined_effect.state == FFPL_STARTED)
				priv->combined_effect.change = FFPL_TO_UPDATE;
			else
				priv->combined_effect.change = FFPL_TO_START;

			return 0;
		}
		/* No combinable effects are active, remove the effect from device */
		if (priv->combined_effect.state != FFPL_EMPTY) {
			printk(KERN_NOTICE "KLGDFF: No combinable effects are active, erase the combined effect from device\n");
			priv->combined_effect.change = FFPL_TO_ERASE;
		}
	}

	return 0;
}

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
	data.effects.repeat = eff->repeat;
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
	data.effects.repeat = eff->repeat;
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

	if (value) {
		eff->change = FFPL_TO_START;
		eff->repeat = value;
	} else
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
		printk(KERN_NOTICE "KLGDFF: Updating effect in slot %d\n", effect->id);
		if (ffpl_needs_replacing(&eff->active, &eff->latest)) {
			eff->replace = true;
			eff->change = FFPL_TO_UPLOAD;
		} else {
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
	int ret;

	s = klgd_alloc_stream();
	if (!s)
		return NULL; /* TODO: Error handling */

	ret = ffpl_handle_combinable_effects(priv, s);
	if (ret)
		printk(KERN_WARNING "KLGDFF: Cannot process combinable effects, ret %d\n", ret);

	for (idx = 0; idx < priv->effect_count; idx++) {
		struct ffpl_effect *eff = &priv->effects[idx];

		printk(KERN_NOTICE "KLGDFF: Processing effect %lu\n", idx);
		ret = ffpl_handle_state_change(priv, s, eff, true);
		/* TODO: Do something useful with the return code */
		if (ret)
			printk(KERN_WARNING "KLGDFF: Cannot get command stream effect %lu\n", idx);
	}

	/* Handle combined effect here */
	ret = ffpl_handle_state_change(priv, s, &priv->combined_effect, false);
	if (ret)
		printk(KERN_WARNING "KLGDFF: Cannot get command stream for combined effect\n");

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

static int ffpl_handle_state_change(struct klgd_plugin_private *priv, struct klgd_command_stream *s, struct ffpl_effect *eff,
				    const bool ignore_combined)
{
	int ret;

	/* Latest effect is of different type than currently active effect,
	 * remove it from the device and upload the latest one */
	if (eff->replace) {
		switch (eff->change) {
		case FFPL_TO_ERASE:
			printk(KERN_NOTICE "KLGDFF: Rpl chg - TO_ERASE\n");
			switch (eff->state) {
			case FFPL_STARTED:
				ret = ffpl_stop_effect(priv, s, eff);
				if (ret)
					break;
			case FFPL_UPLOADED:
				ret = ffpl_erase_effect(priv, s, eff);
				if (ret)
					break;
			default:
				/* Nothing to do - the effect that is replacing the old effect is about to be erased anyway
				 * State of the effect to be replaced should also never be EMPTY */
				ret = 0;
				break;
			}
			break;
		case FFPL_TO_UPLOAD:
		case FFPL_TO_STOP: /* There is no difference between stopping or uploading an effect when we are replacing it */
			printk("KLGDFF: Rpl chg - TO_UPLOAD/TO_STOP\n");
			switch (eff->state) {
			case FFPL_STARTED:
				/* Overwrite the currently active effect and set it to UPLOADED state */
				if (priv->has_owr_to_upl) {
					ret = ffpl_replace_effect(priv, s, eff, FFPL_OWR_TO_UPL);
					break;
				}
				ret = ffpl_stop_effect(priv, s, eff);
				if (ret)
					break;
			case FFPL_UPLOADED:
				ret = ffpl_erase_effect(priv, s, eff);
				if (ret)
					break;
			case FFPL_EMPTY: /* State cannot actually be FFPL_EMPTY becuase only uploaded or started effects have to be replaced like this */
				/* Combinable effects are taken care of elsewhere and should not be uploaded individually */
				if (!ffpl_is_combinable(&eff->latest))
					ret = ffpl_upload_effect(priv, s, eff);
				else
					ret = 0;
				break;
			default:
				printk(KERN_WARNING "KLGDFF: Unhandled effect state\n");
				ret = -EINVAL;
			}
			break;
		case FFPL_TO_START:
		case FFPL_TO_UPDATE: /* There is no difference between staring or updating an effect when we are replacing it */
			printk("KLGDFF: Rpl chg - TO_START/TO_UPDATE\n");
			switch (eff->state) {
			case FFPL_STARTED:
				if (priv->has_owr_to_srt) {
					ret = ffpl_replace_effect(priv, s, eff, FFPL_OWR_TO_SRT);
					break;
				}
				ret = ffpl_stop_effect(priv, s, eff);
				if (ret)
					break;
			case FFPL_UPLOADED:
				ret = ffpl_erase_effect(priv, s, eff);
				if (ret)
					break;
			case FFPL_EMPTY: /* State cannot actually be FFPL_EMPTY - same as above applies */
				/* Combinable effects are taken care of elsewhere and should not be uploaded and started individually */
				if (ffpl_is_combinable(&eff->latest)) {
					ret = 0;
					break;
				}
				ret = ffpl_upload_effect(priv, s, eff);
				if (ret)
					break;
				ret = ffpl_start_effect(priv, s, eff);
				break;
			default:
				printk(KERN_WARNING "KLGDFF: Unhandled effect state\n");
				ret = -EINVAL;
				break;
			}
			break;
		case FFPL_DONT_TOUCH:
			printk(KERN_WARNING "KLGDFF: Got FFPL_DONT_TOUCH change for effect that should be replaced - this should not happen!\n");
			ret = -EINVAL;
			break;
		default:
			printk(KERN_WARNING "KLGDFF: Unhandled state change while replacing effect\n");
			ret = -EINVAL;
			break;
		}
		if (ret)
			printk(KERN_WARNING "KLGDFF: Error %d while replacing effect\n", ret);

		eff->replace = false;
		eff->change = FFPL_DONT_TOUCH;
		return ret;
	}

	/* Combinable effects have already been handled, do not try to handle then again individually */
	if (ffpl_is_combinable(&eff->latest) && ignore_combined) {
		printk(KERN_NOTICE "KLGDFF: Effect is combinable\n");
		ret = 0;
		goto out;
	}

	switch (eff->change) {
	case FFPL_TO_UPLOAD:
		printk(KERN_INFO "KLGDFF: Chg TO_UPLOAD\n");
		switch (eff->state) {
		case FFPL_EMPTY:
			ret = ffpl_upload_effect(priv, s, eff);
			break;
		case FFPL_STARTED:
			ret = ffpl_stop_effect(priv, s, eff);
			break;
		default:
			ret = 0;
			break;
		}
		break;
	case FFPL_TO_START:
		printk(KERN_INFO "KLGDFF: Chg TO_START\n");
		switch (eff->state) {
		case FFPL_EMPTY:
			if (priv->has_emp_to_srt) {
				ret = ffpl_start_effect(priv, s, eff);
				break;
			}
			ret = ffpl_upload_effect(priv, s, eff);
			if (ret)
				return ret;
		case FFPL_UPLOADED:
			ret = ffpl_start_effect(priv, s, eff);
			break;
		default:
			ret = 0;
			break;
		}
		break;
	case FFPL_TO_STOP:
		printk(KERN_INFO "KLGDFF: Chg TO_STOP\n");
		switch (eff->state) {
		case FFPL_STARTED:
			ret = ffpl_stop_effect(priv, s, eff);
			break;
		case FFPL_EMPTY:
			ret = ffpl_upload_effect(priv, s, eff);
			break;
		default:
			ret = 0;
			break;
		}
		break;
	case FFPL_TO_ERASE:
		printk(KERN_INFO "KLGDFF: Chg TO_ERASE\n");
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
			ret = 0;
			break;
		}
		break;
	case FFPL_TO_UPDATE:
		printk(KERN_INFO "KLGDFF: Chg TO_UPDATE\n");
		ret = ffpl_update_effect(priv, s, eff);
		break;
	case FFPL_DONT_TOUCH:
		printk(KERN_INFO "KLGDFF: Chg - NO CHANGE\n");
		return 0;
	default:
		return -EINVAL;
		pr_debug("KLGDFF: Unhandled effect state change\n");
	}

out:
	eff->change = FFPL_DONT_TOUCH;

	return ret;
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
		printk(KERN_NOTICE "KLGDFF: Effects are of different type - replacing (%d x %d)\n", ac_eff->type, la_eff->type);
		return true;
	}

	if (ac_eff->type == FF_PERIODIC) {
		if (ac_eff->u.periodic.waveform != la_eff->u.periodic.waveform) {
			printk(KERN_NOTICE "KLGDFF: Effects have different waveforms - replacing\n");
			return true;
		}
	}

	printk(KERN_NOTICE "KLGDFF: Effect does not have to be replaced, updating\n");
	return false;
}
