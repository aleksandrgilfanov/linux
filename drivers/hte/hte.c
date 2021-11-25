// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2021 NVIDIA Corporation
 *
 * Author: Dipen Patel <dipenp@nvidia.com>
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/err.h>
#include <linux/slab.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/mutex.h>
#include <linux/sched.h>
#include <linux/uaccess.h>
#include <linux/hte.h>
#include <linux/delay.h>
#include <linux/debugfs.h>
#include <linux/kthread.h>

#define HTE_TS_NAME_LEN		10

/* Global list of the HTE devices */
static DEFINE_SPINLOCK(hte_lock);
static LIST_HEAD(hte_devices);

enum {
	HTE_TS_REGISTERED,
	HTE_TS_DISABLE,
};

enum {
	HTE_CB_RUN_THREAD,
	HTE_CB_NUM,
};

/**
 * struct hte_ts_info - Information related to requested timestamp.
 *
 * @xlated_id: Timestamp ID as understood between HTE subsys and HTE provider,
 * See xlate callback API.
 * @flags: Flags holding state informations.
 * @hte_cb_flags: Callback related flags.
 * @seq: Timestamp sequence counter.
 * @hte_name: Indicates if HTE core has set name for this timestamp entity.
 * @cb: Callback function provided by clients.
 * @tcb: Threaded callback function provided by clients.
 * @dropped_ts: Dropped timestamps.
 * @slock: Spin lock.
 * @thread: Thread task when tcb is provided.
 * @req_mlock: Lock during timestamp request/release APIs.
 * @ts_dbg_root: Root for the debug fs.
 * @gdev: HTE abstract device that this timestamp belongs to.
 * @cl_data: Client specific data.
 */
struct hte_ts_info {
	u32 xlated_id;
	unsigned long flags;
	unsigned long hte_cb_flags;
	u64 seq;
	bool hte_name;
	hte_ts_cb_t cb;
	hte_ts_threaded_cb_t tcb;
	atomic_t dropped_ts;
	spinlock_t slock;
	struct task_struct *thread;
	struct mutex req_mlock;
	struct dentry *ts_dbg_root;
	struct hte_device *gdev;
	void *cl_data;
};

/**
 * struct hte_device - HTE abstract device
 * @nlines: Number of entities this device supports.
 * @ts_req: Total number of entities requested.
 * @sdev: Device used at various debug prints.
 * @dbg_root: Root directory for debug fs.
 * @list: List node to store hte_device for each provider.
 * @chip: HTE chip providing this HTE device.
 * @owner: helps prevent removal of modules when in use.
 * @ei: Timestamp information.
 */
struct hte_device {
	u32 nlines;
	atomic_t ts_req;
	struct device *sdev;
	struct dentry *dbg_root;
	struct list_head list;
	struct hte_chip *chip;
	struct module *owner;
	struct hte_ts_info ei[];
};

#ifdef CONFIG_DEBUG_FS

static struct dentry *hte_root;

static int __init hte_subsys_dbgfs_init(void)
{
	/* creates /sys/kernel/debug/hte/ */
	hte_root = debugfs_create_dir("hte", NULL);

	return 0;
}
subsys_initcall(hte_subsys_dbgfs_init);

static void hte_chip_dbgfs_init(struct hte_device *gdev)
{
	const struct hte_chip *chip = gdev->chip;
	const char *name = chip->name ? chip->name : dev_name(chip->dev);

	gdev->dbg_root = debugfs_create_dir(name, hte_root);

	debugfs_create_atomic_t("ts_requested", 0444, gdev->dbg_root,
				&gdev->ts_req);
	debugfs_create_u32("total_ts", 0444, gdev->dbg_root,
			   &gdev->nlines);
}

static void hte_ts_dbgfs_init(const char *name, struct hte_ts_info *ei)
{
	if (!ei->gdev->dbg_root || !name)
		return;

	ei->ts_dbg_root = debugfs_create_dir(name, ei->gdev->dbg_root);

	debugfs_create_atomic_t("dropped_timestamps", 0444, ei->ts_dbg_root,
				&ei->dropped_ts);
}

#else

static void hte_chip_dbgfs_init(struct hte_device *gdev)
{
}

static void hte_ts_dbgfs_init(const char *name, struct hte_ts_info *ei)
{
}

#endif

/**
 * hte_release_ts() - Consumer calls this API to release the entity, where
 * entity could be anything providers support, like lines, signals, buses,
 * etc...
 *
 * @desc: timestamp descriptor, this is the same as returned by the request API.
 *
 * Context: debugfs_remove_recursive() function call may use sleeping locks,
 *	    not suitable from atomic context.
 * Returns: 0 on success or a negative error code on failure.
 */
int hte_release_ts(struct hte_ts_desc *desc)
{
	u32 id;
	int ret = 0;
	unsigned long flag;
	struct hte_device *gdev;
	struct hte_ts_info *ei;

	if (!desc)
		return -EINVAL;

	ei = desc->hte_data;

	if (!ei || !ei->gdev)
		return -EINVAL;

	gdev = ei->gdev;
	id = desc->con_id;

	mutex_lock(&ei->req_mlock);

	if (!test_bit(HTE_TS_REGISTERED, &ei->flags)) {
		dev_info(gdev->sdev, "id:%d is not registered", id);
		ret = -EUSERS;
		goto unlock;
	}

	ret = gdev->chip->ops->release(gdev->chip, ei->xlated_id);
	if (ret) {
		dev_err(gdev->sdev, "id: %d free failed\n", id);
		goto unlock;
	}

	if (ei->hte_name)
		kfree(desc->name);

	debugfs_remove_recursive(ei->ts_dbg_root);

	spin_lock_irqsave(&ei->slock, flag);

	atomic_dec(&gdev->ts_req);
	atomic_set(&ei->dropped_ts, 0);

	ei->seq = 0;
	desc->hte_data = NULL;

	clear_bit(HTE_TS_REGISTERED, &ei->flags);

	spin_unlock_irqrestore(&ei->slock, flag);

	if (ei->tcb) {
		kthread_stop(ei->thread);
		put_task_struct(ei->thread);
	}

	ei->cb = NULL;
	ei->tcb = NULL;
	ei->thread = NULL;
	ei->cl_data = NULL;

	module_put(gdev->owner);
unlock:
	mutex_unlock(&ei->req_mlock);
	dev_dbg(gdev->sdev, "release id: %d\n", id);

	return ret;
}
EXPORT_SYMBOL_GPL(hte_release_ts);

static int hte_ts_dis_en_common(struct hte_ts_desc *desc, bool en)
{
	u32 ts_id;
	struct hte_device *gdev;
	struct hte_ts_info *ei;
	int ret;
	unsigned long flag;

	if (!desc)
		return -EINVAL;

	ei = desc->hte_data;

	if (!ei || !ei->gdev)
		return -EINVAL;

	gdev = ei->gdev;
	ts_id = desc->con_id;

	mutex_lock(&ei->req_mlock);

	if (!test_bit(HTE_TS_REGISTERED, &ei->flags)) {
		dev_dbg(gdev->sdev, "id:%d is not registered", ts_id);
		ret = -EUSERS;
		goto out;
	}

	spin_lock_irqsave(&ei->slock, flag);

	if (en) {
		if (!test_bit(HTE_TS_DISABLE, &ei->flags)) {
			ret = 0;
			goto out_unlock;
		}

		spin_unlock_irqrestore(&ei->slock, flag);
		ret = gdev->chip->ops->enable(gdev->chip, ei->xlated_id);
		if (ret) {
			dev_warn(gdev->sdev, "id: %d enable failed\n",
				 ts_id);
			goto out;
		}

		spin_lock_irqsave(&ei->slock, flag);
		clear_bit(HTE_TS_DISABLE, &ei->flags);
	} else {
		if (test_bit(HTE_TS_DISABLE, &ei->flags)) {
			ret = 0;
			goto out_unlock;
		}

		spin_unlock_irqrestore(&ei->slock, flag);
		ret = gdev->chip->ops->disable(gdev->chip, ei->xlated_id);
		if (ret) {
			dev_warn(gdev->sdev, "id: %d disable failed\n",
				 ts_id);
			goto out;
		}

		spin_lock_irqsave(&ei->slock, flag);
		set_bit(HTE_TS_DISABLE, &ei->flags);
	}

out_unlock:
	spin_unlock_irqrestore(&ei->slock, flag);
out:
	mutex_unlock(&ei->req_mlock);
	return ret;
}

/**
 * hte_disable_ts() - Disable timestamp on given descriptor.
 *
 * The API does not release any resources associated with desc.
 *
 * @desc: ts descriptor, this is the same as returned by the request API.
 *
 * Context: Holds mutex lock, not suitable from atomic context.
 * Returns: 0 on success or a negative error code on failure.
 */
int hte_disable_ts(struct hte_ts_desc *desc)
{
	return hte_ts_dis_en_common(desc, false);
}
EXPORT_SYMBOL_GPL(hte_disable_ts);

/**
 * hte_enable_ts() - Enable timestamp on given descriptor.
 *
 * @desc: ts descriptor, this is the same as returned by the request API.
 *
 * Context: Holds mutex lock, not suitable from atomic context.
 * Returns: 0 on success or a negative error code on failure.
 */
int hte_enable_ts(struct hte_ts_desc *desc)
{
	return hte_ts_dis_en_common(desc, true);
}
EXPORT_SYMBOL_GPL(hte_enable_ts);

static int hte_simple_xlate(struct hte_chip *gc,
			    const struct of_phandle_args *args,
			    struct hte_ts_desc *desc,
			    u32 *id)
{
	if (!id || !desc || !gc)
		return -EINVAL;

	/*
	 * For the providers which do not have any internal mappings between
	 * logically exposed ids and actual ids, will set both
	 * the same.
	 *
	 * In case there is a internal mapping needed, providers will need to
	 * provide its own xlate function where con_id will be sent as
	 * args[0] and it will return xlated id. Later xlated id will be
	 * used for any future exchanges between provider and subsystems.
	 */

	if (args) {
		if (gc->of_hte_n_cells < 1)
			return -EINVAL;

		if (args->args_count != gc->of_hte_n_cells)
			return -EINVAL;

		*id = args->args[0];
		desc->con_id = *id;
	} else {
		*id = desc->con_id;
	}

	if (desc->con_id > gc->nlines)
		return -EINVAL;

	desc->hte_data = NULL;

	return 0;
}

static int _hte_wait_for_ts_data(struct hte_ts_info *ei)
{
	for (;;) {
		set_current_state(TASK_INTERRUPTIBLE);

		if (kthread_should_stop()) {
			if (test_and_clear_bit(HTE_CB_RUN_THREAD,
			    &ei->hte_cb_flags)) {
				__set_current_state(TASK_RUNNING);
				return 0;
			}
			__set_current_state(TASK_RUNNING);
			return -1;
		}

		if (test_and_clear_bit(HTE_CB_RUN_THREAD,
				       &ei->hte_cb_flags)) {
			__set_current_state(TASK_RUNNING);
			return 0;
		}
		schedule();
	}
}

static int _hte_threadfn(void *data)
{
	struct hte_ts_info *ei = data;

	while (!_hte_wait_for_ts_data(ei))
		ei->tcb(ei->cl_data);

	return 0;
}

static int _hte_setup_thread(struct hte_ts_info *ei, u32 id)
{
	struct task_struct *t;

	t = kthread_create(_hte_threadfn, ei, "hte-%u", id);
	if (IS_ERR(t))
		return PTR_ERR(t);

	ei->thread = get_task_struct(t);

	return 0;
}

static int ___hte_req_ts(struct hte_device *gdev, struct hte_ts_desc *desc,
			 u32 xlated_id, hte_ts_cb_t cb,
			 hte_ts_threaded_cb_t tcb, void *data)
{
	struct hte_ts_info *ei;
	int ret;
	u32 con_id = desc->con_id;

	if (!try_module_get(gdev->owner))
		return -ENODEV;

	ei = &gdev->ei[xlated_id];
	ei->xlated_id = xlated_id;

	/*
	 * There is a chance that multiple consumers requesting same entity,
	 * lock here.
	 */
	mutex_lock(&ei->req_mlock);

	if (test_bit(HTE_TS_REGISTERED, &ei->flags)) {
		dev_dbg(gdev->chip->dev, "id:%u is already registered",
			xlated_id);
		ret = -EUSERS;
		goto unlock;
	}

	ei->cb = cb;
	ei->tcb = tcb;
	if (tcb) {
		ret = _hte_setup_thread(ei, xlated_id);
		if (ret < 0) {
			dev_err(gdev->chip->dev, "setting thread failed\n");
			goto unlock;
		}
	}

	ret = gdev->chip->ops->request(gdev->chip, xlated_id);
	if (ret < 0) {
		dev_err(gdev->chip->dev, "ts request failed\n");
		goto unlock;
	}

	desc->hte_data = ei;
	ei->cl_data = data;

	atomic_inc(&gdev->ts_req);

	ei->hte_name = false;
	if (!desc->name) {
		desc->name = kzalloc(HTE_TS_NAME_LEN, GFP_KERNEL);
		if (desc->name) {
			scnprintf(desc->name, HTE_TS_NAME_LEN, "ts_%u",
				  con_id);
			ei->hte_name = true;
		}
	}

	hte_ts_dbgfs_init(desc->name, ei);
	set_bit(HTE_TS_REGISTERED, &ei->flags);

	mutex_unlock(&ei->req_mlock);

	dev_dbg(gdev->chip->dev, "id: %u, xlated id:%u", con_id, xlated_id);

	return 0;

unlock:
	module_put(gdev->owner);
	mutex_unlock(&ei->req_mlock);

	return ret;
}

static struct hte_device *of_node_to_htedevice(struct device_node *np)
{
	struct hte_device *gdev;

	spin_lock(&hte_lock);

	list_for_each_entry(gdev, &hte_devices, list)
		if (gdev->chip && gdev->chip->dev &&
		    gdev->chip->dev->of_node == np) {
			spin_unlock(&hte_lock);
			return gdev;
		}

	spin_unlock(&hte_lock);

	return ERR_PTR(-ENODEV);
}

static struct hte_device *of_hte_dev_get(struct device *dev,
					 struct device_node *np,
					 const char *label,
					 struct of_phandle_args *args)
{
	struct hte_device *gdev = NULL;
	int index = 0;
	int err;

	if (label) {
		index = of_property_match_string(np,
						 "hardware-timestamp-names",
						 label);
		if (index < 0)
			return ERR_PTR(index);
	}

	err = of_parse_phandle_with_args(np, "hardware-timestamps",
					 "#hardware-timestamp-cells", index,
					 args);
	if (err) {
		pr_err("%s(): can't parse \"hardware-timestamps\" property\n",
		       __func__);
		return ERR_PTR(err);
	}

	gdev = of_node_to_htedevice(args->np);
	if (IS_ERR(gdev)) {
		pr_err("%s(): HTE chip not found\n", __func__);
		of_node_put(args->np);
		return gdev;
	}

	return gdev;
}

static int __hte_req_ts(struct device *dev, struct hte_ts_desc *desc,
			hte_ts_cb_t cb, hte_ts_threaded_cb_t tcb, void *data)
{
	struct hte_device *gdev = NULL;
	struct of_phandle_args args;
	int ret;
	u32 xlated_id;

	gdev = of_hte_dev_get(dev, dev->of_node, desc->name, &args);
	if (IS_ERR(gdev))
		return PTR_ERR(gdev);

	if (!gdev->chip) {
		pr_debug("requested id does not have provider\n");
		return -ENODEV;
	}

	ret = gdev->chip->xlate(gdev->chip, &args, desc, &xlated_id);
	if (ret < 0)
		goto put;

	ret = ___hte_req_ts(gdev, desc, xlated_id, cb, tcb, data);
	if (ret < 0)
		goto put;

	return 0;

put:
	of_node_put(args.np);

	return ret;
}

static void __devm_hte_release_ts(void *res)
{
	hte_release_ts(res);
}

/**
 * devm_of_hte_request_ts() - Resource managed API to request the HTE facility
 * on the specified entity, where entity is provider specific for example,
 * GPIO lines, signals, buses etc...
 *
 * The API allocates necessary resources and enables the timestamp. So calling
 * hte_enable_ts is not needed. The consumer does not need to call
 * hte_release_ts since it will be called upon consumer exit.
 *
 * @dev: HTE consumer/client device.
 * @desc: Pre-allocated timestamp descriptor. HTE core will fill out necessary
 * details. Optionally the consumer can set name field of desc, if not
 * specified HTE core will set it as ts_con_id. It will be the consumer's
 * job to free any allocation related to this structure as well name field
 * in case it has set that field.
 * @cb: Callback to push the timestamp data to consumer.
 * @tcb: Optional callback. If its provided, subsystem will create
 * thread. This will be called when cb returns HTE_RUN_THREADED_CB.
 * @data: Client data, will be sent back during cb and tcb callbacks.
 *
 * Context: Holds mutex lock.
 * Returns: Returns 0 on success or negative error code on failure.
 */
int devm_of_hte_request_ts(struct device *dev, struct hte_ts_desc *desc,
			   hte_ts_cb_t cb, hte_ts_threaded_cb_t tcb,
			   void *data)
{
	int err;

	if (!dev || !dev->of_node || !desc || !cb)
		return -EINVAL;

	err = __hte_req_ts(dev, desc, cb, tcb, data);
	if (err)
		return err;

	err = devm_add_action_or_reset(dev, __devm_hte_release_ts, desc);
	if (err)
		return err;

	return 0;
}
EXPORT_SYMBOL_GPL(devm_of_hte_request_ts);

/**
 * hte_req_ts_by_hte_name() - Request entity to timestamp realtime by passing
 * property name that contains HTE provider phandle, meaning of the entity
 * is HTE provider specific, for example lines, signals, GPIOs, buses etc...
 *
 * This API is designed to address below uses cases:
 *
 * 1) For the consumer device which acts as a central device for secondary
 * consumers. For example, GPIO controller driver acts as a primary consumer
 * on behalf of in kernel and userspace GPIO HTE consumers. The GPIO controller
 * driver specifies HTE provider that it supports/wants and it becomes opaque
 * for the secondary consumers requesting GPIO and hardware timestamp through
 * that GPIO controller.
 *
 * 2) For the providers which are dependent on other hardware modules. In that
 * case it forces consumers to go through other subsystem or driver making them
 * secondary consumers. Same example as above applies here as well.
 *
 * The API allocates necessary resources and enables the timestamp. So calling
 * hte_enable_ts is not needed.
 *
 * @dev: HTE consumer/client device.
 * @propname: Name of property holding a HTE provider phandle value
 * @desc: Pre-allocated timestamp descriptor with con_id set by the consumer.
 * HTE core will fill out the rest. Optionally the consumer can set name
 * field of desc, if not specified HTE core will set it as ts_con_id. It will
 * be the consumer's job to free any allocation related to this structure as
 * well name field in case it has set that field.
 * @cb: Callback to push the timestamp data to consumer.
 * @tcb: Optional callback. If its provided, subsystem will create
 * thread. This will be called when cb returns HTE_RUN_THREADED_CB.
 * @data: Client data, will be sent back during cb and tcb callbacks.
 *
 * Context: Holds mutex lock, can not be called from atomic context. The mutex
 * lock is used to serialize multiple consumers.
 * Returns: returns 0 on success or negative error code on failure.
 */
int hte_req_ts_by_hte_name(struct device *dev, const char *propname,
			   struct hte_ts_desc *desc, hte_ts_cb_t cb,
			   hte_ts_threaded_cb_t tcb, void *data)
{
	struct hte_device *gdev;
	struct device_node *np = NULL;
	int ret;
	u32 xlated_id;

	if (!dev->of_node || !propname || !desc)
		return -EINVAL;

	np = of_parse_phandle(dev->of_node, propname, 0);
	if (!np)
		return -ENODEV;

	of_node_put(np);

	gdev = of_node_to_htedevice(np);
	if (IS_ERR(gdev))
		return -ENOTSUPP;

	if (!gdev->chip || !gdev->chip->ops)
		return -ENOTSUPP;

	ret = gdev->chip->xlate(gdev->chip, NULL, desc, &xlated_id);
	if (ret < 0) {
		dev_err(gdev->chip->dev,
			"failed to xlate id: %d\n", desc->con_id);
		return ret;
	}

	ret = ___hte_req_ts(gdev, desc, xlated_id, cb, tcb, data);
	if (ret < 0) {
		dev_err(gdev->chip->dev,
			"failed to request id: %d\n", desc->con_id);
		return ret;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(hte_req_ts_by_hte_name);

/**
 * hte_get_clk_src_info() - Consumer calls this API to query clock source
 * information of the desc.
 *
 * @desc: ts descriptor, same as returned from request API.
 * @ci: The API fills this structure with the clock information data.
 *
 * Context: Any context.
 * Returns: 0 on success else negative error code on failure.
 */
int hte_get_clk_src_info(const struct hte_ts_desc *desc,
			 struct hte_clk_info *ci)
{
	struct hte_chip *chip;
	struct hte_ts_info *ei;

	if (!desc || !desc->hte_data || !ci) {
		pr_debug("%s:%d\n", __func__, __LINE__);
		return -EINVAL;
	}

	ei = desc->hte_data;
	if (!ei || !ei->gdev || !ei->gdev->chip)
		return -EINVAL;

	chip = ei->gdev->chip;
	if (!chip->ops->get_clk_src_info)
		return -ENOTSUPP;

	return chip->ops->get_clk_src_info(chip, ci);
}
EXPORT_SYMBOL_GPL(hte_get_clk_src_info);

/**
 * hte_push_ts_ns() - Used by the provider to push timestamp in nano
 * seconds i.e data->tsc will be in ns.
 *
 * @chip: The HTE chip, used during the registration.
 * @xlated_id: entity id understood by both subsystem and provider, usually this
 * is obtained from xlate callback during request API.
 * @data: timestamp data.
 *
 * Returns: 0 on success or a negative error code on failure.
 */
int hte_push_ts_ns(const struct hte_chip *chip, u32 xlated_id,
		   struct hte_ts_data *data)
{
	hte_return_t ret;
	int st = 0;
	struct hte_ts_info *ei;
	unsigned long flag;

	if (!chip || !data || !chip->gdev)
		return -EINVAL;

	if (xlated_id > chip->nlines)
		return -EINVAL;

	ei = &chip->gdev->ei[xlated_id];

	spin_lock_irqsave(&ei->slock, flag);

	/* timestamp sequence counter */
	data->seq = ei->seq++;

	if (!test_bit(HTE_TS_REGISTERED, &ei->flags) ||
	    test_bit(HTE_TS_DISABLE, &ei->flags)) {
		dev_dbg(chip->dev, "Unknown timestamp push\n");
		st = -EINVAL;
		goto unlock;
	}

	ret = ei->cb(data, ei->cl_data);
	if (ret == HTE_RUN_THREADED_CB && ei->thread) {
		if (test_and_set_bit(HTE_CB_RUN_THREAD, &ei->hte_cb_flags))
			goto unlock;
		else
			wake_up_process(ei->thread);
	} else if (ret == HTE_CB_TS_DROPPED) {
		atomic_inc(&ei->dropped_ts);
	} else if (ret == HTE_CB_ERROR) {
		dev_dbg(chip->dev, "cb error\n");
	}

unlock:
	spin_unlock_irqrestore(&ei->slock, flag);

	return st;
}
EXPORT_SYMBOL_GPL(hte_push_ts_ns);

static int hte_register_chip(struct hte_chip *chip)
{
	struct hte_device *gdev;
	u32 i;

	if (!chip || !chip->dev || !chip->dev->of_node)
		return -EINVAL;

	if (!chip->ops || !chip->ops->request || !chip->ops->release) {
		dev_err(chip->dev, "Driver needs to provide ops\n");
		return -EINVAL;
	}

	gdev = kzalloc(struct_size(gdev, ei, chip->nlines), GFP_KERNEL);
	if (!gdev)
		return -ENOMEM;

	gdev->chip = chip;
	chip->gdev = gdev;
	gdev->nlines = chip->nlines;
	gdev->sdev = chip->dev;

	for (i = 0; i < chip->nlines; i++) {
		gdev->ei[i].gdev = gdev;
		mutex_init(&gdev->ei[i].req_mlock);
		spin_lock_init(&gdev->ei[i].slock);
	}

	if (chip->dev->driver)
		gdev->owner = chip->dev->driver->owner;
	else
		gdev->owner = THIS_MODULE;

	if (!chip->xlate) {
		chip->xlate = hte_simple_xlate;
		/* Just a id number to monitor */
		chip->of_hte_n_cells = 1;
	}

	of_node_get(chip->dev->of_node);

	INIT_LIST_HEAD(&gdev->list);

	spin_lock(&hte_lock);
	list_add_tail(&gdev->list, &hte_devices);
	spin_unlock(&hte_lock);

	hte_chip_dbgfs_init(gdev);

	dev_dbg(chip->dev, "Added hte chip\n");

	return 0;
}

/**
 * hte_unregister_chip() - Used by the provider to remove a HTE chip.
 * @chip: the HTE chip to remove.
 *
 * Context: Can not be called from atomic context.
 * Returns: 0 on success or a negative error code on failure.
 */
static int hte_unregister_chip(struct hte_chip *chip)
{
	struct hte_device *gdev;

	if (!chip)
		return -EINVAL;

	gdev = chip->gdev;

	spin_lock(&hte_lock);
	list_del(&gdev->list);
	spin_unlock(&hte_lock);

	gdev->chip = NULL;

	of_node_put(chip->dev->of_node);
	debugfs_remove_recursive(gdev->dbg_root);
	kfree(gdev);

	dev_dbg(chip->dev, "Removed hte chip\n");

	return 0;
}

static void _hte_devm_unregister_chip(void *chip)
{
	hte_unregister_chip(chip);
}

/**
 * devm_hte_register_chip() - Used by provider to register a HTE chip.
 * @chip: the HTE chip to add to subsystem.
 *
 * The API is resource managed and  _hte_devm_unregister_chip will be called
 * automatically when the provider exits.
 *
 * Returns: 0 on success or a negative error code on failure.
 */
int devm_hte_register_chip(struct hte_chip *chip)
{
	int err;

	err = hte_register_chip(chip);
	if (err)
		return err;

	err = devm_add_action_or_reset(chip->dev, _hte_devm_unregister_chip,
				       chip);
	if (err)
		return err;

	return 0;
}
EXPORT_SYMBOL_GPL(devm_hte_register_chip);
