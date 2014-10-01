#include <linux/input.h>
#include "../KLGD/klgd.h"

#define FFPL_EFBIT(x) BIT(x - FF_EFFECT_MIN)

/* Allowed flag bits */
#define FFPL_HAS_EMP_TO_SRT BIT(0) /* Device supports direct "upload and start" */
#define FFPL_HAS_SRT_TO_EMP BIT(1) /* Device supports direct "stop and erase" */
#define FFPL_UPLOAD_WHEN_STARTED BIT(2) /* Upload effects only when they are started - this implies HAS_EMP_TO_SRT */
#define FFPL_ERASE_WHEN_STOPPED BIT(3) /* Erases effect from device when it is stopped - this implies HAS_SRT_TO_EMP */
#define FFPL_REPLACE_UPLOADED BIT(4) /* Device can accept a new effect to UPLOADED state without the need to explicitly stop and erase the previously uploaded effect beforehand */
#define FFPL_REPLACE_STARTED BIT(5) /* Device can accept a new effect to STARTED state without the need to explicitly stop and erase the previously uploaded effect beforehand */

enum ffpl_control_command {
	/* Force feedback state transitions */
	FFPL_EMP_TO_UPL, /* Upload to empty slot */
	FFPL_UPL_TO_SRT, /* Start uploaded effect */
	FFPL_SRT_TO_UPL, /* Stop started effect */
	FFPL_UPL_TO_EMP, /* Erase uploaded effect */
	FFPL_SRT_TO_UDT, /* Update started effect */
	/* Optional force feedback state transitions */
	FFPL_EMP_TO_SRT, /* Upload and start effect */
	FFPL_SRT_TO_EMP, /* Stop and erase started effect */
	FFPL_OWR_TO_UPL, /* Overwrite an effect with a new one and set its state to UPLOADED */
	FFPL_OWR_TO_SRT	 /* Overwrite an effect with a new one and set its state to STARTED */
};

struct ffpl_effects {
	const struct ff_effect *cur;  /* Pointer to the effect that is being uploaded/started/stopped/erased */
	const struct ff_effect *old;  /* Pointer to the currently active effect. Valid only with OWR_* commands, otherwise NULL */
	int repeat; /* How many times to repeat playback - valid only with *_SRT commands */
};

union ffpl_control_data {
	struct ffpl_effects effects;
	u16 ac_magnitude;
	u16 gain;
};

int ffpl_init_plugin(struct klgd_plugin **plugin, struct input_dev *dev, const size_t effect_count,
		     const unsigned long supported_effects,
		     const unsigned long flags,
		     int (*control)(struct input_dev *dev, struct klgd_command_stream *s, const enum ffpl_control_command cmd, const union ffpl_control_data data));
