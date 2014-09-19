#include <linux/input.h>
#include "../KLGD/klgd.h"

#define FFPL_EFBIT(x) BIT(x - FF_EFFECT_MIN)

/* Allowed flag bits */
#define FFPL_HAS_EMP_TO_SRT BIT(0) /* Device supports direct "upload and start" */
#define FFPL_HAS_SRT_TO_EMP BIT(1) /* Device supports direct "stop and erase" */
#define FFPL_UPLOAD_WHEN_STARTED BIT(2) /* Upload effects only when they are started - this implies HAS_EMP_TO_SRT */
#define FFPL_ERASE_WHEN_STOPPED BIT(3) /* Erases effect from device when it is stopped - this implies HAS_SRT_TO_EMP */

enum ffpl_control_command {
	/* Force feedback state transitions */
	FFPL_EMP_TO_UPL, /* Upload to empty slot */
	FFPL_UPL_TO_SRT, /* Start uploaded effect */
	FFPL_SRT_TO_UPL, /* Stop started effect */
	FFPL_UPL_TO_EMP, /* Erase uploaded effect */
	FFPL_SRT_TO_UDT, /* Update started effect */
	/* Optional force feedback state transitions */
	FFPL_EMP_TO_SRT, /* Upload and start effect */
	FFPL_SRT_TO_EMP  /* Stop and erase started effect */
};

union ffpl_control_data {
	const struct ff_effect *effect;
	u16 ac_magnitude;
	u16 gain;
};

int ffpl_init_plugin(struct klgd_plugin **plugin, struct input_dev *dev, const size_t effect_count,
		     const unsigned long supported_effects,
		     const unsigned long flags,
		     int (*control)(struct input_dev *dev, struct klgd_command_stream *s, const enum ffpl_control_command cmd, const union ffpl_control_data data));
