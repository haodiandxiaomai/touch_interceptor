/*
 * touch_interceptor v4 — 注入测试 (捕获后零开销)
 *
 * 1. 注册一次性 kprobe → 第一个触摸事件时捕获设备指针 → 立即卸载 kprobe
 * 2. 注入通过 workqueue → input_event(real_dev)
 * 3. 零运行时开销 (无 hook，无 trap)
 *
 * 用法:
 *   insmod touch_interceptor.ko
 *   touch 一下屏幕 (触发捕获)
 *   cat /proc/touch_interceptor (确认 real_dev 已捕获)
 *   remote_touch down 540 960
 */

#include <linux/module.h>
#include <linux/input.h>
#include <linux/input/mt.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/proc_fs.h>
#include <linux/workqueue.h>
#include <linux/kprobes.h>

#define MODULE_TAG  "touch_interceptor"
#define PROC_NAME   "touch_interceptor"

/* ============ 状态 ============ */
static struct input_dev *real_dev;
static bool dev_captured;
static u64 stat_injected;

/* ============ 一次性 kprobe ============ */

static struct kprobe kp;

static int capture_handler(struct kprobe *p, struct pt_regs *regs)
{
	struct input_dev *dev;

	if (dev_captured)
		return 0;

	dev = (struct input_dev *)regs->regs[0];

	/* 通过特征匹配: 触摸屏有 ABS_MT_SLOT + INPUT_PROP_DIRECT */
	if (dev && dev->name &&
	    test_bit(ABS_MT_SLOT, dev->absbit) &&
	    test_bit(INPUT_PROP_DIRECT, dev->propbit)) {
		real_dev = dev;
		dev_captured = true;
		pr_info(MODULE_TAG ": captured %p (%s)\n", dev, dev->name);
		unregister_kprobe(&kp);
	}

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
		pr_warn(MODULE_TAG ": no device captured yet, touch screen first\n");
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
		"real_dev: %p (%s)\ncaptured: %s\ninjected: %llu\n",
		real_dev,
		real_dev ? real_dev->name : "none",
		dev_captured ? "yes" : "no",
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

	pr_info(MODULE_TAG ": loading v4\n");

	if (!proc_create(PROC_NAME, 0666, NULL, &proc_fops))
		return -ENOMEM;

	/* 一次性 kprobe: 捕获后自动卸载，零运行时开销 */
	kp.symbol_name = "input_handle_event";
	kp.pre_handler = capture_handler;

	ret = register_kprobe(&kp);
	if (ret) {
		pr_warn(MODULE_TAG ": kprobe failed: %d\n", ret);
		pr_info(MODULE_TAG ": loaded without auto-capture\n");
		pr_info(MODULE_TAG ": use: echo 'dev <name>' > /proc/touch_interceptor\n");
	}

	pr_info(MODULE_TAG ": loaded, touch screen to capture device\n");
	return 0;
}

static void __exit touch_interceptor_exit(void)
{
	if (!dev_captured)
		unregister_kprobe(&kp);
	remove_proc_entry(PROC_NAME, NULL);
	flush_scheduled_work();
	pr_info(MODULE_TAG ": done, injected=%llu\n", stat_injected);
}

module_init(touch_interceptor_init);
module_exit(touch_interceptor_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Touch interceptor v4 — zero-overhead injection");
MODULE_AUTHOR("interceptor");
