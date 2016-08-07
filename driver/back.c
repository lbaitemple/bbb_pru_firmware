#include <linux/module.h> 
#include <linux/device.h>
#include <linux/err.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/slab.h>
#include <linux/kdev_t.h>
#include <linux/remoteproc.h>


static void * Data_pointer; 
int *servo_chain;
#define MAX_servoS 1024

static DEFINE_MUTEX(servo_lookup_lock);
static DEFINE_MUTEX(servo_lock);
static LIST_HEAD(servo_chips);
static DECLARE_BITMAP(allocated_servos, MAX_servoS);


struct servo_export {
	struct device child;
	struct servo_device *servo;
};

struct servo_chip;

/**
 * enum servo_polarity - polarity of a servo signal
 * @servo_POLARITY_NORMAL: a high signal for the duration of the duty-
 * cycle, followed by a low signal for the remainder of the pulse
 * period
 * @servo_POLARITY_INVERSED: a low signal for the duration of the duty-
 * cycle, followed by a high signal for the remainder of the pulse
 * period
 */
enum servo_polarity {
	servo_POLARITY_NORMAL,
	servo_POLARITY_INVERSED,
};

enum {
	SERVOF_REQUESTED = 1 << 0,
	SERVOF_ENABLED = 1 << 1,
	SERVOF_EXPORTED = 1 << 2,
};

/**
 * struct servo_device - servo channel object
 * @label: name of the servo device
 * @flags: flags associated with the servo device
 * @hwservo: per-chip relative index of the servo device
 * @servo: global index of the servo device
 * @chip: servo chip providing this servo device
 * @chip_data: chip-private data associated with the servo device
 * @lock: used to serialize accesses to the servo device where necessary
 * @period: period of the servo signal (in nanoseconds)
 * @duty_cycle: duty cycle of the servo signal (in nanoseconds)
 * @polarity: polarity of the servo signal
 */
struct servo_device {
	const char *label;
	unsigned long flags;
	unsigned int hwservo;
	unsigned int servo;
	struct servo_chip *chip;
	void *chip_data;
	struct mutex lock;

	unsigned int period;
	unsigned int duty_cycle;
	enum servo_polarity polarity;
};

static inline bool servo_is_enabled(const struct servo_device *servo)
{
	return test_bit(SERVOF_ENABLED, &servo->flags);
}

static inline void servo_set_period(struct servo_device *servo, unsigned int period)
{
	if (servo)
		servo->period = period;
}

static inline unsigned int servo_get_period(const struct servo_device *servo)
{
	return servo ? servo->period : 0;
}

static inline void servo_set_duty_cycle(struct servo_device *servo, unsigned int duty)
{
	if (servo)
		servo->duty_cycle = duty;
}

static inline unsigned int servo_get_duty_cycle(const struct servo_device *servo)
{
	return servo ? servo->duty_cycle : 0;
}

/*
 * servo_set_polarity - configure the polarity of a servo signal
 */
int servo_set_polarity(struct servo_device *servo, enum servo_polarity polarity);

static inline enum servo_polarity servo_get_polarity(const struct servo_device *servo)
{
	return servo ? servo->polarity : servo_POLARITY_NORMAL;
}

/**
 * struct servo_ops - servo controller operations
 * @request: optional hook for requesting a servo
 * @free: optional hook for freeing a servo
 * @config: configure duty cycles and period length for this servo
 * @set_polarity: configure the polarity of this servo
 * @enable: enable servo output toggling
 * @disable: disable servo output toggling
 * @dbg_show: optional routine to show contents in debugfs
 * @owner: helps prevent removal of modules exporting active servos
 */
struct servo_ops {
	int (*request)(struct servo_chip *chip, struct servo_device *servo);
	void (*free)(struct servo_chip *chip, struct servo_device *servo);
	int (*config)(struct servo_chip *chip, struct servo_device *servo,
		      int duty_ns, int period_ns);
	int (*set_polarity)(struct servo_chip *chip, struct servo_device *servo,
			    enum servo_polarity polarity);
	int (*enable)(struct servo_chip *chip, struct servo_device *servo);
	void (*disable)(struct servo_chip *chip, struct servo_device *servo);
#ifdef CONFIG_DEBUG_FS
	void (*dbg_show)(struct servo_chip *chip, struct seq_file *s);
#endif
	struct module *owner;
};

/**
 * struct servo_chip - abstract a servo controller
 * @dev: device providing the servos
 * @list: list node for internal use
 * @ops: callbacks for this servo controller
 * @base: number of first servo controlled by this chip
 * @nservo: number of servos controlled by this chip
 * @servos: array of servo devices allocated by the framework
 * @of_xlate: request a servo device given a device tree servo specifier
 * @of_servo_n_cells: number of cells expected in the device tree servo specifier
 * @can_sleep: must be true if the .config(), .enable() or .disable()
 *             operations may sleep
 */
struct servo_chip {
	struct device *dev;
	struct list_head list;
	const struct servo_ops *ops;
	int base;
	unsigned int nservo;

	struct servo_device *servos;

	struct servo_device * (*of_xlate)(struct servo_chip *pc,
					const struct of_phandle_args *args);
	unsigned int of_servo_n_cells;
	bool can_sleep;
};




/**
 * servo_config() - change a servo device configuration
 * @servo: servo device
 * @duty_ns: "on" time (in nanoseconds)
 * @period_ns: duration (in nanoseconds) of one cycle
 *
 * Returns: 0 on success or a negative error code on failure.
 */
int servo_config(struct servo_device *servo, int duty_ns, int period_ns)
{
	int err;

	if (!servo || duty_ns < 0 || period_ns <= 0 || duty_ns > period_ns)
		return -EINVAL;

	err = servo->chip->ops->config(servo->chip, servo, duty_ns, period_ns);
	if (err)
		return err;

	servo->duty_cycle = duty_ns;
	servo->period = period_ns;

	return 0;
}
EXPORT_SYMBOL_GPL(servo_config);

/**
 * servo_set_polarity() - configure the polarity of a servo signal
 * @servo: servo device
 * @polarity: new polarity of the servo signal
 *
 * Note that the polarity cannot be configured while the servo device is
 * enabled.
 *
 * Returns: 0 on success or a negative error code on failure.
 */
int servo_set_polarity(struct servo_device *servo, enum servo_polarity polarity)
{
	int err;

	if (!servo || !servo->chip->ops)
		return -EINVAL;

	if (!servo->chip->ops->set_polarity)
		return -ENOSYS;

	mutex_lock(&servo->lock);

	if (servo_is_enabled(servo)) {
		err = -EBUSY;
		goto unlock;
	}

	err = servo->chip->ops->set_polarity(servo->chip, servo, polarity);
	if (err)
		goto unlock;

	servo->polarity = polarity;

unlock:
	mutex_unlock(&servo->lock);
	return err;
}
EXPORT_SYMBOL_GPL(servo_set_polarity);

/**
 * servo_enable() - start a servo output toggling
 * @servo: servo device
 *
 * Returns: 0 on success or a negative error code on failure.
 */
int servo_enable(struct servo_device *servo)
{
	int err = 0;

	if (!servo)
		return -EINVAL;

	mutex_lock(&servo->lock);

	if (!test_and_set_bit(SERVOF_ENABLED, &servo->flags)) {
		err = servo->chip->ops->enable(servo->chip, servo);
		if (err)
			clear_bit(SERVOF_ENABLED, &servo->flags);
	}

	mutex_unlock(&servo->lock);

	return err;
}
EXPORT_SYMBOL_GPL(servo_enable);

/**
 * servo_disable() - stop a servo output toggling
 * @servo: servo device
 */
void servo_disable(struct servo_device *servo)
{
	if (servo && test_and_clear_bit(SERVOF_ENABLED, &servo->flags))
		servo->chip->ops->disable(servo->chip, servo);
}
EXPORT_SYMBOL_GPL(servo_disable);




static struct servo_export *child_to_servo_export(struct device *child)
{
	return container_of(child, struct servo_export, child);
}

static struct servo_device *child_to_servo_device(struct device *child)
{
	struct servo_export *export = child_to_servo_export(child);

	return export->servo;
}


static ssize_t period_show(struct device *child,
			   struct device_attribute *attr,
			   char *buf)
{
	const struct servo_device *servo = child_to_servo_device(child);

	return sprintf(buf, "%u\n", servo_get_period(servo));
}

static ssize_t period_store(struct device *child,
			    struct device_attribute *attr,
			    const char *buf, size_t size)
{
	struct servo_device *servo = child_to_servo_device(child);
	unsigned int val;
	int ret;

	ret = kstrtouint(buf, 0, &val);
	if (ret)
		return ret;

	ret = servo_config(servo, servo_get_duty_cycle(servo), val);

	return ret ? : size;
}

static ssize_t duty_cycle_show(struct device *child,
			       struct device_attribute *attr,
			       char *buf)
{
	const struct servo_device *servo = child_to_servo_device(child);

	return sprintf(buf, "%u\n", servo_get_duty_cycle(servo));
}

static ssize_t duty_cycle_store(struct device *child,
				struct device_attribute *attr,
				const char *buf, size_t size)
{
	struct servo_device *servo = child_to_servo_device(child);
	unsigned int val;
	int ret;

	ret = kstrtouint(buf, 0, &val);
	if (ret)
		return ret;

	ret = servo_config(servo, val, servo_get_period(servo));

	return ret ? : size;
}

static ssize_t enable_show(struct device *child,
			   struct device_attribute *attr,
			   char *buf)
{
	const struct servo_device *servo = child_to_servo_device(child);

	return sprintf(buf, "%d\n", servo_is_enabled(servo));
}

static ssize_t enable_store(struct device *child,
			    struct device_attribute *attr,
			    const char *buf, size_t size)
{
	struct servo_device *servo = child_to_servo_device(child);
	int val, ret;

	ret = kstrtoint(buf, 0, &val);
	if (ret)
		return ret;

	switch (val) {
	case 0:
		servo_disable(servo);
		break;
	case 1:
		ret = servo_enable(servo);
		break;
	default:
		ret = -EINVAL;
		break;
	}

	return ret ? : size;
}

static ssize_t polarity_show(struct device *child,
			     struct device_attribute *attr,
			     char *buf)
{
	const struct servo_device *servo = child_to_servo_device(child);
	const char *polarity = "unknown";

	switch (servo_get_polarity(servo)) {
	case servo_POLARITY_NORMAL:
		polarity = "normal";
		break;

	case servo_POLARITY_INVERSED:
		polarity = "inversed";
		break;
	}

	return sprintf(buf, "%s\n", polarity);
}

static ssize_t polarity_store(struct device *child,
			      struct device_attribute *attr,
			      const char *buf, size_t size)
{
	struct servo_device *servo = child_to_servo_device(child);
	enum servo_polarity polarity;
	int ret;

	if (sysfs_streq(buf, "normal"))
		polarity = servo_POLARITY_NORMAL;
	else if (sysfs_streq(buf, "inversed"))
		polarity = servo_POLARITY_INVERSED;
	else
		return -EINVAL;

	ret = servo_set_polarity(servo, polarity);

	return ret ? : size;
}

/**
 * servo_put() - release a servo device
 * @servo: servo device
 */
void servo_put(struct servo_device *servo)
{
	if (!servo)
		return;

	mutex_lock(&servo_lock);

	if (!test_and_clear_bit(SERVOF_REQUESTED, &servo->flags)) {
		pr_warn("servo device already freed\n");
		goto out;
	}

	if (servo->chip->ops->free)
		servo->chip->ops->free(servo->chip, servo);

	servo->label = NULL;

	module_put(servo->chip->ops->owner);
out:
	mutex_unlock(&servo_lock);
}
EXPORT_SYMBOL_GPL(servo_put);


static int servo_device_request(struct servo_device *servo, const char *label)
{
	int err;

	if (test_bit(SERVOF_REQUESTED, &servo->flags))
		return -EBUSY;

	if (!try_module_get(servo->chip->ops->owner))
		return -ENODEV;

	if (servo->chip->ops->request) {
		err = servo->chip->ops->request(servo->chip, servo);
		if (err) {
			module_put(servo->chip->ops->owner);
			return err;
		}
	}

	set_bit(SERVOF_REQUESTED, &servo->flags);
	servo->label = label;

	return 0;
}


/**
 * servo_request_from_chip() - request a servo device relative to a servo chip
 * @chip: servo chip
 * @index: per-chip index of the servo to request
 * @label: a literal description string of this servo
 *
 * Returns: A pointer to the servo device at the given index of the given servo
 * chip. A negative error code is returned if the index is not valid for the
 * specified servo chip or if the servo device cannot be requested.
 */
struct servo_device *servo_request_from_chip(struct servo_chip *chip,
					 unsigned int index,
					 const char *label)
{
	struct servo_device *servo;
	int err;

	if (!chip || index >= chip->nservo)
		return ERR_PTR(-EINVAL);

	mutex_lock(&servo_lock);
	servo = &chip->servos[index];

	err = servo_device_request(servo, label);
	if (err < 0)
		servo = ERR_PTR(err);

	mutex_unlock(&servo_lock);
	return servo;
}
EXPORT_SYMBOL_GPL(servo_request_from_chip);

static DEVICE_ATTR_RW(period);
static DEVICE_ATTR_RW(duty_cycle);
static DEVICE_ATTR_RW(enable);
static DEVICE_ATTR_RW(polarity);

static struct attribute *servo_attrs[] = {
	&dev_attr_period.attr,
	&dev_attr_duty_cycle.attr,
	&dev_attr_enable.attr,
	&dev_attr_polarity.attr,
	NULL
};
ATTRIBUTE_GROUPS(servo);

static void servo_export_release(struct device *child)
{
	struct servo_export *export = child_to_servo_export(child);

	kfree(export);
}

static int servo_export_child(struct device *parent, struct servo_device *servo)
{
	struct servo_export *export;
	int ret;

	if (test_and_set_bit(SERVOF_EXPORTED, &servo->flags))
		return -EBUSY;

	export = kzalloc(sizeof(*export), GFP_KERNEL);
	if (!export) {
		clear_bit(SERVOF_EXPORTED, &servo->flags);
		return -ENOMEM;
	}

	export->servo = servo;

	export->child.release = servo_export_release;
	export->child.parent = parent;
	export->child.devt = MKDEV(0, 0);
	export->child.groups = servo_groups;
	dev_set_name(&export->child, "servo%u", servo->hwservo);

	ret = device_register(&export->child);
	if (ret) {
		clear_bit(SERVOF_EXPORTED, &servo->flags);
		kfree(export);
		return ret;
	}

	return 0;
}

static int servo_unexport_match(struct device *child, void *data)
{
	return child_to_servo_device(child) == data;
}

static int servo_unexport_child(struct device *parent, struct servo_device *servo)
{
	struct device *child;

	if (!test_and_clear_bit(SERVOF_EXPORTED, &servo->flags))
		return -ENODEV;

	child = device_find_child(parent, servo, servo_unexport_match);
	if (!child)
		return -ENODEV;

	/* for device_find_child() */
	put_device(child);
	device_unregister(child);
	servo_put(servo);

	return 0;
}

static ssize_t export_store(struct device *parent,
			    struct device_attribute *attr,
			    const char *buf, size_t len)
{
	struct servo_chip *chip = dev_get_drvdata(parent);
	struct servo_device *servo;
	unsigned int hwservo;
	int ret;

	ret = kstrtouint(buf, 0, &hwservo);
	if (ret < 0)
		return ret;

	if (hwservo >= chip->nservo)
		return -ENODEV;

	servo = servo_request_from_chip(chip, hwservo, "sysfs");
	if (IS_ERR(servo))
		return PTR_ERR(servo);

	ret = servo_export_child(parent, servo);
	if (ret < 0)
		servo_put(servo);

	return ret ? : len;
}
static DEVICE_ATTR_WO(export);

static ssize_t unexport_store(struct device *parent,
			      struct device_attribute *attr,
			      const char *buf, size_t len)
{
	struct servo_chip *chip = dev_get_drvdata(parent);
	unsigned int hwservo;
	int ret;

	ret = kstrtouint(buf, 0, &hwservo);
	if (ret < 0)
		return ret;

	if (hwservo >= chip->nservo)
		return -ENODEV;

	ret = servo_unexport_child(parent, &chip->servos[hwservo]);

	return ret ? : len;
}
static DEVICE_ATTR_WO(unexport);

static ssize_t nservo_show(struct device *parent, struct device_attribute *attr,
			 char *buf)
{
	const struct servo_chip *chip = dev_get_drvdata(parent);

	return sprintf(buf, "%u\n", chip->nservo);
}
static DEVICE_ATTR_RO(nservo);

static struct attribute *servo_chip_attrs[] = {
	&dev_attr_export.attr,
	&dev_attr_unexport.attr,
	&dev_attr_nservo.attr,
	NULL,
};
ATTRIBUTE_GROUPS(servo_chip);

static struct class servo_class = {
	.name = "servo",
	.owner = THIS_MODULE,
	.dev_groups = servo_chip_groups,
	//.probe = 
};

static int servochip_sysfs_match(struct device *parent, const void *data)
{
	return dev_get_drvdata(parent) == data;
}

void servochip_sysfs_export(struct servo_chip *chip)
{
	struct device *parent;

	/*
	 * If device_create() fails the servo_chip is still usable by
	 * the kernel its just not exported.
	 */
	parent = device_create(&servo_class, chip->dev, MKDEV(0, 0), chip,
			       "servochip%d", chip->base);
	if (IS_ERR(parent)) {
		dev_warn(chip->dev,
			 "device_create failed for servo_chip sysfs export\n");
	}
}

void servochip_sysfs_unexport(struct servo_chip *chip)
{
	struct device *parent;

	parent = class_find_device(&servo_class, NULL, chip,
				   servochip_sysfs_match);
	if (parent) {
		/* for class_find_device() */
		put_device(parent);
		device_unregister(parent);
	}
}


static int __init servo_sysfs_init(void)
{
	 //Allocate memory for I/O.
    request_mem_region(0x4a310000, 32, "Data");
    
    //Ioremap returns a virtual address in Data_pointer.
    Data_pointer=ioremap(0x4a310000, 32);
    
    //Allocate memeory to *mosi
    servo_chain=kmalloc(8*sizeof(int), GFP_KERNEL);

	return class_register(&servo_class);
}



static void __exit servo_sysfs_exit(void)
{
    //device_destroy(cl, device1);
    //class_destroy(cl);
    //cdev_del(&spi_pru);
    //unregister_chrdev_region(device1, 1);
    release_mem_region(0x4a310000, 32);
    iounmap(Data_pointer);
    Data_pointer=NULL;
    servo_chain=NULL;
}

module_init(servo_sysfs_init);
module_exit(servo_sysfs_exit);

MODULE_AUTHOR("Kiran Kumar Lekkala <kiran4399@gmail.com>");
MODULE_DESCRIPTION("Driver for PRU servo");
MODULE_LICENSE("GPL v2");
