
/* State change flags */
enum ffpl_st_change {
	FFPL_DONT_TOUCH,  /* Effect has not been changed since last update */
	FFPL_TO_UPLOAD,	  /* Effect shall be uploaded to device */
	FFPL_TO_START,	  /* Effect shall be started */
	FFPL_TO_STOP,	  /* Effect shall be stopped */
	FFPL_TO_ERASE,	  /* Effect shall be removed from device */
	FFPL_TO_UPDATE	  /* Effect paramaters shall be updated */
};

/* Status flags */
enum ffpl_state {
	FFPL_EMPTY,	  /* There is no effect in the slot */
	FFPL_UPLOADED,	  /* Effect in the slot is uploaded to device */
	FFPL_STARTED,	  /* Effect in the slot is started on device */
};

struct ffpl_effect {
	struct ff_effect *active;	/* Last effect submitted to device */
	struct ff_effect *latest;	/* Last effect submitted to us by userspace */
	enum ffpl_st_change change;	/* State to which the effect shall be put */
	enum ffpl_state state;		/* State of the active effect */
	bool replace;			/* Active effect has to be replaced => active effect shall be erased and latest uploaded */
};

struct klgd_plugin_private {
	struct ffpl_effect *effects;
	unsigned long supported_effects;
	size_t effect_count;
	u16 gain;
	struct input_dev *dev;
	struct klgd_command * (*upload_effect)(struct input_dev *dev, const struct ff_effect *effect, const int id);
	struct klgd_command * (*start_effect)(struct input_dev *dev, const struct ff_effect *effect, const int id);
	struct klgd_command * (*stop_effect)(struct input_dev *dev, const struct ff_effect *effect, const int id);
	struct klgd_command * (*erase_effect)(struct input_dev *dev, const struct ff_effect *effect, const int id);
};
