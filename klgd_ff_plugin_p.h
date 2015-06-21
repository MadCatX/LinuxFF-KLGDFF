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
	FFPL_STARTED,	  /* Effect in the slot is started on device */
};

struct ffpl_effect {
	struct ff_effect active;	/* Last effect submitted to device */
	struct ff_effect latest;	/* Last effect submitted to us by userspace */
	int repeat;			/* How many times to repeat an effect - set in playback_rq */
	enum ffpl_st_change change;	/* State to which the effect shall be put */
	enum ffpl_state state;		/* State of the active effect */
	bool replace;			/* Active effect has to be replaced => active effect shall be erased and latest uploaded */
	bool uploaded_to_device;	/* Effect was physically uploaded to device */
};

struct klgd_plugin_private {
	struct ffpl_effect *effects;
	struct ffpl_effect combined_effect;
	unsigned long supported_effects;
	size_t effect_count;
	struct input_dev *dev;
	int (*control)(struct input_dev *dev, struct klgd_command_stream *s, const enum ffpl_control_command cmd, const union ffpl_control_data data);
	/* Optional device capabilities */
	bool has_emp_to_srt;
	bool has_srt_to_emp;
	bool upload_when_started;
	bool erase_when_stopped;
	bool has_owr_to_upl;
	bool has_owr_to_srt;
	u32 padding:26;
};
