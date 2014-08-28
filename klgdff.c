#include <linux/delay.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/sysfs.h>
#include "klgd_ff_plugin.h"

#define EFFECT_COUNT 8

static struct kobject *klgdff_obj;

static struct input_dev *dev;
static struct klgd_main klgd;
static struct klgd_plugin *ff_plugin;

static struct klgd_command * klgdff_erase(struct input_dev *dev, const struct ff_effect *effect, const int id)
{
	char *text = kasprintf(GFP_KERNEL, "Erasing effect, type %d, id %d", effect->type, id);
	size_t len = strlen(text);
	struct klgd_command *c = klgd_alloc_cmd(len + 1);
	memcpy(c->bytes, text, len);
	kfree(text);
	return c;
}

static struct klgd_command * klgdff_start(struct input_dev *de, const struct ff_effect *effect, const int id)
{
	char *text = kasprintf(GFP_KERNEL, "Playing effect, type %d, id %d", effect->type, id);
	size_t len = strlen(text);
	struct klgd_command *c = klgd_alloc_cmd(len + 1);
	memcpy(c->bytes, text, len);
	kfree(text);
	return c;
}

static struct klgd_command * klgdff_stop(struct input_dev *dev, const struct ff_effect *effect, const int id)
{
	char *text = kasprintf(GFP_KERNEL, "Stopping effect, type %d, id %d", effect->type, id);
	size_t len = strlen(text);
	struct klgd_command *c = klgd_alloc_cmd(len + 1);
	memcpy(c->bytes, text, len);
	kfree(text);
	return c;
}

static struct klgd_command * klgdff_upload(struct input_dev *dev, const struct ff_effect *effect, const int id)
{
	char *text = kasprintf(GFP_KERNEL, "Uploading effect, type %d, id %d", effect->type, id);
	size_t len = strlen(text);
	struct klgd_command *c = klgd_alloc_cmd(len + 1);
	memcpy(c->bytes, text, len);
	kfree(text);
	return c;
}

int klgdff_callback(void *data, const struct klgd_command_stream *s)
{
	size_t idx;

	printk(KERN_NOTICE "KLGDTM - EFF...\n");
	for (idx = 0; idx < s->count; idx++)
		printk(KERN_NOTICE "KLGDTM - EFF %s\n", s->commands[idx]->bytes);

	usleep_range(7500, 8500);

	return 0;
}

static void __exit klgdff_exit(void)
{
	input_unregister_device(dev);
	klgd_deinit(&klgd);
	kobject_put(klgdff_obj);
	printk(KERN_NOTICE "KLGD FF sample module removed\n");
}

static int __init klgdff_init(void)
{
	unsigned long ffbits = FFPL_EFBIT(FF_CONSTANT) | FFPL_EFBIT(FF_RUMBLE);
	int ret;

	klgdff_obj = kobject_create_and_add("klgdff_obj", kernel_kobj);
	if (!klgdff_obj)
		return -ENOMEM;

	ret = klgd_init(&klgd, NULL, klgdff_callback, 1);
	if (ret) {
		printk(KERN_ERR "Cannot initialize KLGD\n");
		goto errout_klgd;
	}

	dev = input_allocate_device();
	if (!dev) {
		ret = -ENODEV;
		printk(KERN_ERR "Cannot allocate input device\n");
		goto errout_idev;
	}
	dev->id.bustype = BUS_VIRTUAL;
	dev->id.vendor = 0xffff;
	dev->id.product = 0x8807;
	dev->id.version = 0x8807;
	dev->name = kasprintf(GFP_KERNEL, "KLGD-FF TestModule");
	dev->uniq = kasprintf(GFP_KERNEL, "KLGD-FF TestModule-X");
	dev->dev.parent = NULL;

	input_set_capability(dev, EV_ABS, ABS_X);
	input_set_capability(dev, EV_ABS, ABS_Y);
	input_set_capability(dev, EV_KEY, BTN_0);
	input_set_capability(dev, EV_KEY, BTN_TRIGGER);
	input_set_abs_params(dev, ABS_X, -0x7fff, 0x7fff, 0, 0);
	input_set_abs_params(dev, ABS_Y, -0x7fff, 0x7fff, 0, 0);

	ret = ffpl_init_plugin(&ff_plugin, dev, EFFECT_COUNT, ffbits,
			       klgdff_upload, klgdff_start, klgdff_stop, klgdff_erase);
	if (ret) {
		printk(KERN_ERR "KLGDFF: Cannot init plugin\n");
		goto errout_idev;
	}
      	ret = input_register_device(dev);
	if (ret) {
		printk(KERN_ERR "Cannot register input device\n");
		goto errout_regdev;
	}
	
	ret = klgd_register_plugin(&klgd, 0, ff_plugin, true);
	if (ret) {
		printk(KERN_ERR "KLGDFF: Cannot register plugin\n");
		goto errout_idev;
	}



	printk(KERN_NOTICE "KLGD FF sample module loaded\n");
	return 0;

errout_regdev:
	input_free_device(dev);
errout_idev:
	klgd_deinit(&klgd);
errout_klgd:
	kobject_put(klgdff_obj);
	return ret;
}

module_exit(klgdff_exit)
module_init(klgdff_init);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Michal \"MadCatX\" Maly");
MODULE_DESCRIPTION("KLGD FF TestModule");


