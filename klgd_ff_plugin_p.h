
/* State change flags */
enum ffpl_st_change {
	FFPL_DONT_TOUCH,
	FFPL_TO_UPLOAD,
	FFPL_TO_START,
	FFPL_TO_STOP,
	FFPL_TO_ERASE,
	FFPL_TO_UPDATE
};

/* Status flags */
enum ffpl_state {
	FFPL_EMPTY,
	FFPL_UPLOADED,
	FFPL_STARTED,
};

struct ffpl_effect {
	struct ff_effect *active;
	struct ff_effect *latest;
	enum ffpl_st_change change;
	enum ffpl_state state;
	bool replace;
};

struct klgd_plugin_private {
	struct ffpl_effect *effects;
	unsigned long supported_effects;
	size_t effect_count;
	struct input_dev *dev;
	struct klgd_command * (*upload_effect)(const struct ff_effect *effect, const int id);
	struct klgd_command * (*start_effect)(const struct ff_effect *effect, const int id);
	struct klgd_command * (*stop_effect)(const struct ff_effect *effect, const int id);
	struct klgd_command * (*erase_effect)(const struct ff_effect *effect, const int id);
};
