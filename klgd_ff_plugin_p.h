#include "klgd_ff_plugin.h"

/* Possible state changes of an effect */
enum ffpl_st_change {
	FFPL_DONT_TOUCH,  /* Effect has not been changed since last update */
	FFPL_TO_UPLOAD,	  /* Effect shall be uploaded to device */
	FFPL_TO_START,	  /* Effect shall be started */
	FFPL_TO_STOP,	  /* Effect shall be stopped */
	FFPL_TO_ERASE,	  /* Effect shall be removed from device */
	FFPL_TO_UPDATE	  /* Effect paramaters shall be updated */
};

/* Possible states of an effect */
enum ffpl_state {
	FFPL_EMPTY,	  /* There is no effect in the slot */
	FFPL_UPLOADED,	  /* Effect in the slot is uploaded to device */
	FFPL_STARTED	  /* Effect in the slot is started on device */
};

/* What to do at the next timing trip point */
enum ffpl_trigger {
	FFPL_TRIG_NONE,	    /* No timing event scheduled for and effect */
	FFPL_TRIG_NOW,	    /* State change has been set elsewhere and is to be processed immediately */
	FFPL_TRIG_START,    /* Effect is to be started */
	FFPL_TRIG_RESTART,  /* Effect is to be restarted */
	FFPL_TRIG_STOP,	    /* Effect is to be stopped */
	FFPL_TRIG_RECALC,   /* Effect needs to be recalculated */
	FFPL_TRIG_UPDATE    /* Effect needs to be updated */
};

struct ffpl_effect {
	struct ff_effect active;	/* Last effect submitted to device */
	struct ff_effect latest;	/* Last effect submitted to us by userspace */
	enum ffpl_st_change change;	/* State to which the effect shall be put */
	enum ffpl_state state;		/* State of the active effect */
	bool replace;			/* Active effect has to be replaced => active effect shall be erased and latest uploaded */
	bool uploaded_to_device;	/* Effect was physically uploaded to device */

	enum ffpl_trigger trigger;	/* What to do with the effect at its nearest timing trip point */
	int repeat;			/* How many times to repeat an effect - set in playback_rq */
	unsigned long start_at;		/* Time when to start the effect - in jiffies */
	unsigned long stop_at;		/* Time when to stop the effect - in jiffies */
	unsigned long updated_at;	/* Time when the effect was recalculated last time - in jiffies */
	unsigned long touch_at;		/* Time of the next modification of the effect - in jiffies */
	u16 playback_time;		/* Used internally by effect processor to calculate periods */
	bool recalculate;		/* Effect shall be recalculated in the respective processing loop */
};

struct klgd_plugin_private {
	struct ffpl_effect *effects;
	struct ffpl_effect combined_effect;
	unsigned long supported_effects;
	size_t effect_count;
	struct input_dev *dev;
	int (*control)(struct input_dev *dev, struct klgd_command_stream *s, const enum ffpl_control_command cmd, const union ffpl_control_data data, void *user);
	void *user;
	u16 gain;
	u16 autocenter;
	/* Optional device capabilities */
	bool has_emp_to_srt;
	bool has_srt_to_emp;
	bool upload_when_started;
	bool erase_when_stopped;
	bool has_owr_to_upl;
	bool has_owr_to_srt;
	bool has_native_gain;
	bool has_autocenter;
	bool memless_constant;
	bool memless_periodic;
	bool memless_ramp;
	bool timing_condition;
	u32 padding_caps:20;
	/* Device-wide state changes */
	bool change_gain;
	bool change_autocenter;
	u32 padding_dw:30;
};
