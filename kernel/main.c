/*
 * touch_interceptor v5 — 零 hook, 零开销
 *
 * 1. kprobe on kallsyms_lookup_name (触发1次 → 获取地址 → 卸载)
 * 2. kallsyms_lookup_name("input_class") → class_find_device 遍历设备
 * 3. 找到匹配名称的 input_dev
 * 4. 注入通过 workqueue → input_event(real_dev)
 *
 * 运行时零 hook 开销: 不拦截真实事件, 不 hook 任何热函数
 */

#include <linux/module.h>
#include <linux/input.h>
#include <linux/input/mt.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/proc_fs.h>
#include <linux/workqueue.h>
#include <linux/kprobes.h>
#include <linux/device.h>

#define MODULE_TAG  "touch_interceptor"
#define PROC_NAME   "touch_interceptor"
#define TARGET_NAME "NVTCapacitiveTouchScreen"

/* ============ 参数 ============ */
static char target_name[128] = TARGET_NAME;
module_param_string(target, target_name, sizeof(target_name), 0644);

/* ============ 状态 ============ */
static struct input_dev *real_dev;
static u64 stat_injected;

/* kallsyms_lookup_name 地址 (kprobe trick 获取) */
typedef unsigned long (*kallsyms_lookup_name_t)(const char *name);
static kallsyms_lookup_name_t my_kallsyms_lookup_name;

/* ============ Step 1: 获取 kallsyms_lookup_name 地址 ============ */

static struct kprobe kp_kallsyms;

static int kp_kallsyms_handler(struct kprobe *p, struct pt_regs *regs)
{
	my_kallsyms_lookup_name = (kallsyms_lookup_name_t)p->addr;
	return 0;
}

/* ============ Step 2: 通过 input_class 找设备 ============ */

struct find_ctx {
	const char *name;
	struct input_dev *found;
};

static int match_device(struct device *dev, const void *data)
{
	struct find_ctx *ctx = data;
	struct input_dev *idev;

	/* input_dev 中嵌入的 device 是 dev->parent */
	if (!dev->parent)
		return 0;

	idev = container_of(dev->parent, struct input_dev, dev);
	if (idev && idev->name && strcmp(idev->name, ctx->name) == 0) {
		ctx->found = idev;
		return 1; /* 找到了, 停止遍历 */
	}
	return 0;
}

static int find_touchscreen(void)
{
	struct class *input_class;
	struct find_ctx ctx = { .name = target_name, .found = NULL };

	if (!my_kallsyms_lookup_name) {
		pr_err(MODULE_TAG ": kallsyms_lookup_name not available\n");
		return -ENODEV;
	}

	input_class = (struct class *)my_kallsyms_lookup_name("input_class");
	if (!input_class) {
		pr_err(MODULE_TAG ": input_class not found\n");
		return -ENODEV;
	}

	class_find_device(input_class, NULL, &ctx, match_device);

	if (!ctx.found) {
		pr_err(MODULE_TAG ": device '%s' not found\n", target_name);
		return -ENODEV;
	}

	real_dev = ctx.found;
	pr_info(MODULE_TAG ": found %s at %p\n", real_dev->name, real_dev);
	return 0;
}

/* ============ 注入工作队列 ============ */

struct touch_cmd {
	u32 cmd;
	s32 x;
	s32 y;
	u32 param;
};

struct inject_work {
	struct work_struct work;
	struct touch_cmd cmd;
};

static void inject_fn(struct work_struct *w)
{
	struct inject_work *iw = container_of(w, struct inject_work, work);
	struct input_dev *dev = real_dev;
	struct touch_cmd *c = &iw->cmd;

	if (!dev) {
		kfree(iw);
		return;
	}

	switch (c->cmd) {
	case 0: /* DOWN */
		input_report_key(dev, BTN_TOUCH, 1);
		input_report_abs(dev, ABS_MT_SLOT, 0);
		input_report_abs(dev, ABS_MT_TRACKING_ID, 1);
		input_report_abs(dev, ABS_MT_POSITION_X, c->x);
		input_report_abs(dev, ABS_MT_POSITION_Y, c->y);
		input_mt_sync(dev);
		input_sync(dev);
		break;
	case 1: /* MOVE */
		input_report_abs(dev, ABS_MT_SLOT, 0);
		input_report_abs(dev, ABS_MT_POSITION_X, c->x);
		input_report_abs(dev, ABS_MT_POSITION_Y, c->y);
		input_report_key(dev, BTN_TOUCH, 1);
		input_mt_sync(dev);
		input_sync(dev);
		break;
	case 2: /* UP */
		input_report_abs(dev, ABS_MT_SLOT, 0);
		input_report_abs(dev, ABS_MT_TRACKING_ID, -1);
		input_report_key(dev, BTN_TOUCH, 0);
		input_mt_sync(dev);
		input_sync(dev);
		break;
	case 10: /* MT_DOWN */
		input_report_abs(dev, ABS_MT_SLOT, c->param);
		input_report_abs(dev, ABS_MT_TRACKING_ID, c->param + 1);
		input_report_abs(dev, ABS_MT_POSITION_X, c->x);
		input_report_abs(dev, ABS_MT_POSITION_Y, c->y);
		break;
	case 11: /* MT_MOVE */
		input_report_abs(dev, ABS_MT_SLOT, c->param);
		input_report_abs(dev, ABS_MT_POSITION_X, c->x);
		input_report_abs(dev, ABS_MT_POSITION_Y, c->y);
		break;
	case 12: /* MT_UP */
		input_report_abs(dev, ABS_MT_SLOT, c->param);
		input_report_abs(dev, ABS_MT_TRACKING_ID, -1);
		break;
	case 13: /* MT_SYNC */
		input_mt_sync_frame(dev);
		input_sync(dev);
		break;
	}

	stat_injected++;
	kfree(iw);
}

static void schedule_inject(struct touch_cmd *cmd)
{
	struct inject_work *iw;

	if (!real_dev) {
		pr_warn(MODULE_TAG ": no device\n");
		return;
	}

	iw = kmalloc(sizeof(*iw), GFP_KERNEL);
	if (!iw)
		return;

	INIT_WORK(&iw->work, inject_fn);
	iw->cmd = *cmd;
	schedule_work(&iw->work);
}

/* ============ proc 接口 ============ */

static ssize_t proc_write(struct file *file, const char __user *buf,
			  size_t count, loff_t *ppos)
{
	struct touch_cmd cmd;
	if (count < sizeof(cmd)) return -EINVAL;
	if (copy_from_user(&cmd, buf, sizeof(cmd))) return -EFAULT;
	schedule_inject(&cmd);
	return count;
}

static ssize_t proc_read(struct file *file, char __user *buf,
			 size_t count, loff_t *ppos)
{
	char info[256];
	int len;
	if (*ppos > 0) return 0;

	len = snprintf(info, sizeof(info),
		"target: %s\nreal_dev: %p (%s)\ninjected: %llu\n",
		target_name, real_dev,
		real_dev ? real_dev->name : "none",
		stat_injected);

	if (len > count) len = count;
	if (copy_to_user(buf, info, len)) return -EFAULT;
	*ppos += len;
	return len;
}

static const struct proc_ops proc_fops = {
	.proc_read  = proc_read,
	.proc_write = proc_write,
};

/* ============ 模块入口/出口 ============ */

static int __init touch_interceptor_init(void)
{
	int ret;

	pr_info(MODULE_TAG ": loading v5 (zero hook)\n");

	/* Step 1: kprobe on kallsyms_lookup_name — 仅触发一次 */
	kp_kallsyms.symbol_name = "kallsyms_lookup_name";
	kp_kallsyms.pre_handler = kp_kallsyms_handler;

	ret = register_kprobe(&kp_kallsyms);
	if (ret) {
		pr_err(MODULE_TAG ": kallsyms kprobe failed: %d\n", ret);
		return ret;
	}
	unregister_kprobe(&kp_kallsyms);
	pr_info(MODULE_TAG ": kallsyms_lookup_name = %p\n",
		my_kallsyms_lookup_name);

	/* Step 2: 通过 input_class 找设备 */
	ret = find_touchscreen();
	if (ret) {
		pr_err(MODULE_TAG ": touchscreen not found\n");
		return ret;
	}

	/* Step 3: 创建 proc */
	if (!proc_create(PROC_NAME, 0666, NULL, &proc_fops))
		return -ENOMEM;

	pr_info(MODULE_TAG ": loaded v5, device: %s\n", real_dev->name);
	return 0;
}

static void __exit touch_interceptor_exit(void)
{
	remove_proc_entry(PROC_NAME, NULL);
	flush_scheduled_work();
	pr_info(MODULE_TAG ": done, injected=%llu\n", stat_injected);
}

module_init(touch_interceptor_init);
module_exit(touch_interceptor_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Touch interceptor v5 — zero hook overhead");
MODULE_AUTHOR("interceptor");
