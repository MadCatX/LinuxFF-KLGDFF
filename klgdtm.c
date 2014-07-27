#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/sysfs.h>
#include "../KLGD/klgd.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Michal \"MadCatX\" Maly");
MODULE_DESCRIPTION("...");

static ssize_t klgdtm_num_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf);
static ssize_t klgdtm_num_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count);

static struct kobject *klgdtm_obj;
static int klgdtm_number;

static struct klgd_main klgd;

static struct kobj_attribute put_num_attr = 
	__ATTR(num, 0666, klgdtm_num_show, klgdtm_num_store);

static struct attribute *attrs[] = {
	&put_num_attr.attr,
	NULL
};

static struct attribute_group attrs_grp = {
	.attrs = attrs
};

/* KLGD plugin */

struct klgd_plugin_private {
	int old_n;
	int n;
};

static struct klgd_plugin_private plugin_private = {
	0
};

static struct klgd_command_stream * klgdtm_plugin_get_commands(struct klgd_plugin *self, const unsigned long now)
{
	struct klgd_plugin_private *p = self->private;
	struct klgd_command_stream *s;
	struct klgd_command *c;

	s = kzalloc(sizeof(struct klgd_command_stream), GFP_KERNEL);
	if (!s)
		return NULL;
	s->commands = kzalloc(sizeof(struct klgd_command *) * 2, GFP_KERNEL);
	if (!s->commands) {
		kfree(s);
		return NULL;
	}

	c = kzalloc(sizeof(struct klgd_command), GFP_KERNEL);
	if (!c)
		return NULL;

	c->bytes = kzalloc(sizeof(int), GFP_KERNEL);
	if (!c->bytes) {
		kfree(s->commands);
		kfree(s);
		kfree(c);
		return NULL;
	}
	c->length = sizeof(int);

	*(int*)(c->bytes) = p->n;
	*(int*)(c->bytes + sizeof(int)) = p->old_n;

	s->commands[0] = c;
	s->count = 1;

	p->old_n = p->n;

	return s;
}

bool klgdtm_plugin_get_update_time(struct klgd_plugin *self, const unsigned long now, unsigned long *t)
{
	struct klgd_plugin_private *p = self->private;

	if (p->n) {
		*t = now + msecs_to_jiffies(1000);
		return true;
	}
	return false;
}


int klgdtm_plugin_post_event(struct klgd_plugin *self, void *data)
{
	struct klgd_plugin_private *p = self->private;
	int num = *(int*)data;

	p->n = num;
	if (num == 0)
		p->old_n = 0;

	return 0;
}

static struct klgd_plugin klgdtm_plugin = {
	&plugin_private,
	NULL,
	klgdtm_plugin_get_commands,
	klgdtm_plugin_get_update_time,
	NULL,
	NULL,
	klgdtm_plugin_post_event
};


/** END of plugin definition */

static enum klgd_send_status klgdtm_callback(void *data, struct klgd_command_stream *s)
{
	int num, old_num;
	if (!s) {
		printk(KERN_NOTICE "Empty stream\n");
		return KLGD_SS_DONE;
	}
	if (s->commands[0]->length != sizeof(int)) {
		printk(KERN_WARNING "Malformed stream\n");
		return KLGD_SS_FAILED;
	}

	num = *(int*)s->commands[0]->bytes;
	old_num = *(int*)(s->commands[0]->bytes + sizeof(int));
	printk(KERN_NOTICE "KLGDTM: Value %d, prev: %d\n", num, old_num);

	return KLGD_SS_DONE;
}

static ssize_t klgdtm_num_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%d\n", klgdtm_number);
}

static ssize_t klgdtm_num_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count)
{
	int n;
	sscanf(buf, "%d", &n);
	klgdtm_number = n;

	klgd_post_event(&klgd, 0, &n);
	return count;
}

static void __exit klgdtm_exit(void)
{
	klgd_deinit(&klgd);
	sysfs_remove_group(klgdtm_obj, &attrs_grp);
	kobject_put(klgdtm_obj);
	printk(KERN_NOTICE "KLGD sample module removed\n");
}

static int __init klgdtm_init(void)
{
	int ret;

	klgdtm_obj = kobject_create_and_add("klgdtm_obj", kernel_kobj);
	if (!klgdtm_obj)
		return -ENOMEM;

	ret = sysfs_create_group(klgdtm_obj, &attrs_grp);
	if (ret)
		goto errout_sysfs;

	ret = klgd_init(&klgd, NULL, klgdtm_callback, 1);
	if (ret) {
		printk(KERN_ERR "Cannot initialize KLGD\n");
		goto errout_klgd;
	}

	klgd_register_plugin(&klgd, 0, &klgdtm_plugin);

	printk(KERN_NOTICE "KLGD sample module loaded\n");
	return 0;

errout_klgd:
	sysfs_remove_group(klgdtm_obj, &attrs_grp);
errout_sysfs:
	kobject_put(klgdtm_obj);
	return ret;
}

module_exit(klgdtm_exit)
module_init(klgdtm_init);
