/*
 * touch_interceptor v3 — 纯 kprobe 方案
 *
 * 不创建虚拟设备，不注册 input handler。
 * 一个 kprobe hook input_handle_event，搞定一切。
 *
 * 真实事件：kprobe 拦截 → 计数/可选丢弃
 * 注入事件：inject_to_real=true → kprobe 放行 → evdev → Android
 */

#include <linux/module.h>
#include <linux/input.h>
#include <linux/input/mt.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/proc_fs.h>
#include <linux/kprobes.h>

#define MODULE_TAG  "touch_interceptor"
#define PROC_NAME   "touch_interceptor"
#define TARGET_NAME "NVTCapacitiveTouchScreen"

/* ============ 配置 ============ */
static char target_device[128] = TARGET_NAME;
module_param_string(target, target_device, sizeof(target_device), 0644);
MODULE_PARM_DESC(target, "Name of touchscreen to intercept");

/* ============ 状态 ============ */
static struct input_dev *real_dev;
static bool intercepting = true;
static bool inject_to_real;
static u64 stat_intercepted;
static u64 stat_injected;

/* ============ kprobe: hook input_handle_event ============ */

/*
 * arm64 寄存器: x0=dev, x1=type, x2=code, x3=value
 * 返回 0=放行, !0=跳过(丢弃事件)
 */
static int kp_handler(struct kprobe *p, struct pt_regs *regs)
{
	struct input_dev *dev = (struct input_dev *)regs->regs[0];

	/* 注入模式：放行 */
	if (inject_to_real)
		return 0;

	/* 捕获真实设备指针 */
	if (!real_dev && dev && dev->name &&
	    strcmp(dev->name, target_device) == 0) {
		real_dev = dev;
		pr_info(MODULE_TAG ": captured dev %p (%s)\n", dev, dev->name);
	}

	/* 拦截真实触摸 */
	if (dev == real_dev && intercepting) {
		stat_intercepted++;
		return !0;
	}

	return 0;
}

static struct kprobe kp = {
	.symbol_name = "input_handle_event",
	.pre_handler = kp_handler,
};

/* ============ 注入 ============ */

struct touch_cmd {
	u32 cmd;
	s32 x;
	s32 y;
	u32 param;
};

#define CMD_DOWN     0
#define CMD_MOVE     1
#define CMD_UP       2
#define CMD_MT_DOWN  10
#define CMD_MT_MOVE  11
#define CMD_MT_UP    12
#define CMD_MT_SYNC  13

static void inject_touch(struct touch_cmd *cmd)
{
	struct input_dev *dev = real_dev;

	if (!dev) {
		pr_warn(MODULE_TAG ": no real_dev yet\n");
		return;
	}

	inject_to_real = true;

	switch (cmd->cmd) {
	case CMD_DOWN:
		input_report_key(dev, BTN_TOUCH, 1);
		input_report_abs(dev, ABS_MT_SLOT, 0);
		input_report_abs(dev, ABS_MT_TRACKING_ID, 1);
		input_report_abs(dev, ABS_MT_POSITION_X, cmd->x);
		input_report_abs(dev, ABS_MT_POSITION_Y, cmd->y);
		input_mt_sync(dev);
		input_sync(dev);
		break;
	case CMD_MOVE:
		input_report_abs(dev, ABS_MT_SLOT, 0);
		input_report_abs(dev, ABS_MT_POSITION_X, cmd->x);
		input_report_abs(dev, ABS_MT_POSITION_Y, cmd->y);
		input_report_key(dev, BTN_TOUCH, 1);
		input_mt_sync(dev);
		input_sync(dev);
		break;
	case CMD_UP:
		input_report_abs(dev, ABS_MT_SLOT, 0);
		input_report_abs(dev, ABS_MT_TRACKING_ID, -1);
		input_report_key(dev, BTN_TOUCH, 0);
		input_mt_sync(dev);
		input_sync(dev);
		break;
	case CMD_MT_DOWN:
		input_report_abs(dev, ABS_MT_SLOT, cmd->param);
		input_report_abs(dev, ABS_MT_TRACKING_ID, cmd->param + 1);
		input_report_abs(dev, ABS_MT_POSITION_X, cmd->x);
		input_report_abs(dev, ABS_MT_POSITION_Y, cmd->y);
		break;
	case CMD_MT_MOVE:
		input_report_abs(dev, ABS_MT_SLOT, cmd->param);
		input_report_abs(dev, ABS_MT_POSITION_X, cmd->x);
		input_report_abs(dev, ABS_MT_POSITION_Y, cmd->y);
		break;
	case CMD_MT_UP:
		input_report_abs(dev, ABS_MT_SLOT, cmd->param);
		input_report_abs(dev, ABS_MT_TRACKING_ID, -1);
		break;
	case CMD_MT_SYNC:
		input_mt_sync_frame(dev);
		input_sync(dev);
		break;
	}

	inject_to_real = false;
	stat_injected++;
}

/* ============ proc 接口 ============ */

static ssize_t proc_write(struct file *file, const char __user *buf,
			  size_t count, loff_t *ppos)
{
	struct touch_cmd cmd;
	if (count < sizeof(cmd)) return -EINVAL;
	if (copy_from_user(&cmd, buf, sizeof(cmd))) return -EFAULT;
	inject_touch(&cmd);
	return count;
}

static ssize_t proc_read(struct file *file, char __user *buf,
			 size_t count, loff_t *ppos)
{
	char info[512];
	int len;
	if (*ppos > 0) return 0;

	len = snprintf(info, sizeof(info),
		"target: %s\nreal_dev: %p\nintercepting: %s\n"
		"intercepted: %llu\ninjected: %llu\n",
		target_device, real_dev,
		intercepting ? "yes" : "no",
		stat_intercepted, stat_injected);

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
	pr_info(MODULE_TAG ": loading v3 (pure kprobe)\n");

	ret = register_kprobe(&kp);
	if (ret) {
		pr_err(MODULE_TAG ": kprobe failed: %d\n", ret);
		return ret;
	}

	if (!proc_create(PROC_NAME, 0666, NULL, &proc_fops)) {
		unregister_kprobe(&kp);
		return -ENOMEM;
	}

	pr_info(MODULE_TAG ": loaded v3\n");
	return 0;
}

static void __exit touch_interceptor_exit(void)
{
	remove_proc_entry(PROC_NAME, NULL);
	unregister_kprobe(&kp);
	pr_info(MODULE_TAG ": done, int=%llu inj=%llu\n",
		stat_intercepted, stat_injected);
}

module_init(touch_interceptor_init);
module_exit(touch_interceptor_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Touch interceptor v3 — pure kprobe");
MODULE_AUTHOR("interceptor");
