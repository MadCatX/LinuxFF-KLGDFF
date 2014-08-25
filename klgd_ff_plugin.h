#include <linux/input.h>
#include "../KLGD/klgd.h"

#define FFPL_EFBIT(x) BIT(x - FF_EFFECT_MIN)

int ffpl_init_plugin(struct klgd_plugin **plugin, struct input_dev *dev, const size_t effect_count,
		     const unsigned long supported_effects,
		     struct klgd_command * (*upload)(struct input_dev *dev, const struct ff_effect *effect, const int id),
		     struct klgd_command * (*play)(struct input_dev *dev, const struct ff_effect *effect, const int id),
		     struct klgd_command * (*stop)(struct input_dev *dev, const struct ff_effect *effect, const int id),
		     struct klgd_command * (*erase)(struct input_dev *dev, const struct ff_effect *effect, const int id));
