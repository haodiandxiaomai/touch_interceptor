/*
 * touch_interceptor — 统一触摸拦截模块 v2
 *
 * 双模式注入：
 *   1. vdev 注入（filter 拦截真实事件，vdev 输出）
 *   2. kprobe 注入（hook input_handle_event，直接往真实设备注入）
 *
 * 真实事件：filter 拦截 → vdev → 用户空间 (event9)
 * 注入事件：vdev 输出 (event9) + kprobe 直写 (event5)
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

#include "intercept.h"

#define MODULE_TAG "touch_interceptor"

/* ============ 配置 ============ */
static char target_device[128] = TARGET_DEVICE_NAME;
module_param_string(target, target_device, sizeof(target_device), 0644);
MODULE_PARM_DESC(target, "Name of the touchscreen device to intercept");

/* ============ 全局状态 ============ */
static struct interceptor_ctx ctx;

/* ============ kprobe 注入: hook input_handle_event ============ */

/*
 * input_handle_event 是 input 事件处理的中枢。
 * 通过 kprobe 拦截，可以获取真实设备的 dev 指针，
 * 并在注入时绕过 filter。
 *
 * 调用链：
 *   input_event() → spin_lock → input_handle_event() → input_pass_values()
 *
 * 注入时设置 inject_to_real 标志，kprobe 返回 0 (NOP)
 * 让原始 input_handle_event 正常执行，事件直达 evdev。
 */

static int kp_input_handle_event_entry(struct kprobe *p, struct pt_regs *regs)
{
	struct input_dev *dev;
	unsigned int type, code;
	int value;

	/*
	 * arm64 寄存器:
	 *   x0 = dev, x1 = type, x2 = code, x3 = value
	 */
	dev = (struct input_dev *)regs->regs[0];
	type = (unsigned int)regs->regs[1];
	code = (unsigned int)regs->regs[2];
	value = (int)regs->regs[3];

	/* 注入模式：放行，让 input_handle_event 正常处理 */
	if (ctx.inject_to_real) {
		return 0;
	}

	/* 识别真实触摸设备 */
	if (!ctx.real_dev && dev && dev->name &&
	    strcmp(dev->name, target_device) == 0) {
		ctx.real_dev = dev;
		pr_info(MODULE_TAG ": kprobe captured real dev: %p (%s)\n",
			dev, dev->name);
	}

	/* 真实事件重定向到 vdev */
	if (dev == ctx.real_dev && ctx.vdev && ctx.intercepting) {
		/* EV_SYN 时做同步 */
		if (type == EV_SYN && code == SYN_REPORT) {
			input_mt_sync_frame(ctx.vdev);
			input_sync(ctx.vdev);
			ctx.stats.intercepted++;
			return 0; /* 阻止原始事件 */
		}

		/* 转发到 vdev */
		input_event(ctx.vdev, type, code, value);
		ctx.stats.intercepted++;
		return 0; /* 阻止原始事件 */
	}

	return 0; /* 其他设备不干预 */
}

static struct kprobe kp_input_handle_event = {
	.symbol_name = "input_handle_event",
	.pre_handler = kp_input_handle_event_entry,
};

/* ============ 虚拟设备创建 ============ */

static int create_virtual_device(struct input_dev *real_dev)
{
	struct input_dev *vdev;
	int slot_max, x_max, y_max;
	int i, ret;

	vdev = input_allocate_device();
	if (!vdev)
		return -ENOMEM;

	vdev->name = VIRTUAL_DEVICE_NAME;
	vdev->phys = "virtual/interceptor";
	vdev->id.bustype = BUS_VIRTUAL;
	vdev->id.vendor  = 0x0000;
	vdev->id.product = 0x0001;
	vdev->id.version = 0x0001;

	/* 复制事件类型 */
	for (i = 0; i < EV_CNT; i++) {
		if (test_bit(i, real_dev->evbit))
			__set_bit(i, vdev->evbit);
	}

	/* 复制所有按键能力 — 关键！Android 靠 BTN_TOUCH 识别触摸屏 */
	for (i = 0; i < KEY_CNT; i++) {
		if (test_bit(i, real_dev->keybit))
			__set_bit(i, vdev->keybit);
	}

	/* 复制 MSC 能力 */
	for (i = 0; i < MSC_CNT; i++) {
		if (test_bit(i, real_dev->mscbit))
			__set_bit(i, vdev->mscbit);
	}

	/* 复制 ABS 参数 */
	if (test_bit(ABS_MT_SLOT, real_dev->absbit)) {
		slot_max = input_abs_get_max(real_dev, ABS_MT_SLOT);
		input_set_abs_params(vdev, ABS_MT_SLOT, 0, slot_max, 0, 0);
	}
	if (test_bit(ABS_MT_TRACKING_ID, real_dev->absbit)) {
		input_set_abs_params(vdev, ABS_MT_TRACKING_ID, 0,
				     input_abs_get_max(real_dev, ABS_MT_TRACKING_ID), 0, 0);
	}
	if (test_bit(ABS_MT_POSITION_X, real_dev->absbit)) {
		x_max = input_abs_get_max(real_dev, ABS_MT_POSITION_X);
		input_set_abs_params(vdev, ABS_MT_POSITION_X, 0, x_max, 0, 0);
	}
	if (test_bit(ABS_MT_POSITION_Y, real_dev->absbit)) {
		y_max = input_abs_get_max(real_dev, ABS_MT_POSITION_Y);
		input_set_abs_params(vdev, ABS_MT_POSITION_Y, 0, y_max, 0, 0);
	}
	if (test_bit(ABS_MT_TOUCH_MAJOR, real_dev->absbit)) {
		input_set_abs_params(vdev, ABS_MT_TOUCH_MAJOR, 0,
				     input_abs_get_max(real_dev, ABS_MT_TOUCH_MAJOR), 0, 0);
	}
	if (test_bit(ABS_MT_WIDTH_MAJOR, real_dev->absbit)) {
		input_set_abs_params(vdev, ABS_MT_WIDTH_MAJOR, 0,
				     input_abs_get_max(real_dev, ABS_MT_WIDTH_MAJOR), 0, 0);
	}
	if (test_bit(ABS_MT_PRESSURE, real_dev->absbit)) {
		input_set_abs_params(vdev, ABS_MT_PRESSURE, 0,
				     input_abs_get_max(real_dev, ABS_MT_PRESSURE), 0, 0);
	}

	/* 复制属性 */
	for (i = 0; i < INPUT_PROP_CNT; i++) {
		if (test_bit(i, real_dev->propbit))
			__set_bit(i, vdev->propbit);
	}

	ret = input_register_device(vdev);
	if (ret) {
		input_free_device(vdev);
		return ret;
	}

	ctx.vdev = vdev;
	pr_info(MODULE_TAG ": virtual device created: %s (mirrored from %s)\n",
		vdev->name, real_dev->name);
	return 0;
}

static void destroy_virtual_device(void)
{
	if (ctx.vdev) {
		input_unregister_device(ctx.vdev);
		ctx.vdev = NULL;
	}
}

/* ============ input handler: 拦截真实事件 ============ */

static bool interceptor_filter(struct input_handle *handle,
			       unsigned int type, unsigned int code, int value)
{
	unsigned long flags;

	if (handle != ctx.real_handle)
		return false;

	if (!ctx.vdev)
		return false;

	if (!ctx.intercepting)
		return false;

	/* 转发到虚拟设备 */
	spin_lock_irqsave(&ctx.event_lock, flags);
	input_event(ctx.vdev, type, code, value);
	spin_unlock_irqrestore(&ctx.event_lock, flags);

	ctx.stats.intercepted++;
	return true; /* 阻止原始事件 */
}

static int interceptor_connect(struct input_handler *handler,
			       struct input_dev *dev,
			       const struct input_device_id *id)
{
	struct input_handle *handle;
	int ret;

	if (strcmp(dev->name, target_device) != 0)
		return -ENODEV;

	pr_info(MODULE_TAG ": found target device: %s\n", dev->name);

	handle = kzalloc(sizeof(*handle), GFP_KERNEL);
	if (!handle)
		return -ENOMEM;

	handle->dev = dev;
	handle->handler = handler;
	handle->name = "touch_interceptor";

	ret = input_register_handle(handle);
	if (ret) {
		kfree(handle);
		return ret;
	}

	ret = input_open_device(handle);
	if (ret) {
		input_unregister_handle(handle);
		kfree(handle);
		return ret;
	}

	ctx.real_handle = handle;
	ctx.real_dev = dev;

	ret = create_virtual_device(dev);
	if (ret) {
		pr_err(MODULE_TAG ": failed to create vdev: %d\n", ret);
		input_close_device(handle);
		input_unregister_handle(handle);
		kfree(handle);
		ctx.real_handle = NULL;
		ctx.real_dev = NULL;
		return ret;
	}

	ctx.intercepting = true;

	pr_info(MODULE_TAG ": interception active (filter + kprobe dual mode)\n");
	return 0;
}

static void interceptor_disconnect(struct input_handle *handle)
{
	pr_info(MODULE_TAG ": device disconnected\n");
	ctx.intercepting = false;
	destroy_virtual_device();
	ctx.real_handle = NULL;
	/* 不清除 ctx.real_dev，kprobe 可能仍需要它 */
	input_close_device(handle);
	input_unregister_handle(handle);
	kfree(handle);
}

static const struct input_device_id interceptor_ids[] = {
	{
		.flags = INPUT_DEVICE_ID_MATCH_EVBIT | INPUT_DEVICE_ID_MATCH_ABSBIT,
		.evbit = { BIT_MASK(EV_ABS) },
		.absbit = { BIT_MASK(ABS_MT_SLOT) },
	},
	{ },
};

static struct input_handler interceptor_handler = {
	.filter     = interceptor_filter,
	.connect    = interceptor_connect,
	.disconnect = interceptor_disconnect,
	.name       = MODULE_TAG,
	.id_table   = interceptor_ids,
};

/* ============ 远程注入接口 (/proc/touch_interceptor) ============ */

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

/*
 * 注入到 vdev (事件会到达 event9)
 */
static void inject_to_vdev(struct touch_cmd *cmd)
{
	unsigned long flags;
	struct input_dev *vdev = ctx.vdev;

	if (!vdev)
		return;

	spin_lock_irqsave(&ctx.event_lock, flags);

	switch (cmd->cmd) {
	case CMD_DOWN:
		input_report_key(vdev, BTN_TOUCH, 1);
		input_report_abs(vdev, ABS_MT_SLOT, 0);
		input_report_abs(vdev, ABS_MT_TRACKING_ID, 1);
		input_report_abs(vdev, ABS_MT_POSITION_X, cmd->x);
		input_report_abs(vdev, ABS_MT_POSITION_Y, cmd->y);
		input_mt_sync(vdev);
		input_sync(vdev);
		break;
	case CMD_MOVE:
		input_report_abs(vdev, ABS_MT_SLOT, 0);
		input_report_abs(vdev, ABS_MT_POSITION_X, cmd->x);
		input_report_abs(vdev, ABS_MT_POSITION_Y, cmd->y);
		input_report_key(vdev, BTN_TOUCH, 1);
		input_mt_sync(vdev);
		input_sync(vdev);
		break;
	case CMD_UP:
		input_report_abs(vdev, ABS_MT_SLOT, 0);
		input_report_abs(vdev, ABS_MT_TRACKING_ID, -1);
		input_report_key(vdev, BTN_TOUCH, 0);
		input_mt_sync(vdev);
		input_sync(vdev);
		break;
	case CMD_MT_DOWN:
		input_report_abs(vdev, ABS_MT_SLOT, cmd->param);
		input_report_abs(vdev, ABS_MT_TRACKING_ID, cmd->param + 1);
		input_report_abs(vdev, ABS_MT_POSITION_X, cmd->x);
		input_report_abs(vdev, ABS_MT_POSITION_Y, cmd->y);
		break;
	case CMD_MT_MOVE:
		input_report_abs(vdev, ABS_MT_SLOT, cmd->param);
		input_report_abs(vdev, ABS_MT_POSITION_X, cmd->x);
		input_report_abs(vdev, ABS_MT_POSITION_Y, cmd->y);
		break;
	case CMD_MT_UP:
		input_report_abs(vdev, ABS_MT_SLOT, cmd->param);
		input_report_abs(vdev, ABS_MT_TRACKING_ID, -1);
		break;
	case CMD_MT_SYNC:
		input_mt_sync_frame(vdev);
		input_sync(vdev);
		break;
	default:
		break;
	}

	spin_unlock_irqrestore(&ctx.event_lock, flags);
}

/*
 * 注入到真实设备 (事件直接到达 event5，绕过 filter)
 * 通过 kprobe 注入标志，让 input_handle_event 放行
 */
static void inject_to_real(struct touch_cmd *cmd)
{
	struct input_dev *rdev = ctx.real_dev;

	if (!rdev)
		return;

	/* 设置注入标志 — kprobe 看到后会放行 */
	ctx.inject_to_real = true;

	switch (cmd->cmd) {
	case CMD_DOWN:
		input_report_key(rdev, BTN_TOUCH, 1);
		input_report_abs(rdev, ABS_MT_SLOT, 0);
		input_report_abs(rdev, ABS_MT_TRACKING_ID, 1);
		input_report_abs(rdev, ABS_MT_POSITION_X, cmd->x);
		input_report_abs(rdev, ABS_MT_POSITION_Y, cmd->y);
		input_mt_sync(rdev);
		input_sync(rdev);
		break;
	case CMD_MOVE:
		input_report_abs(rdev, ABS_MT_SLOT, 0);
		input_report_abs(rdev, ABS_MT_POSITION_X, cmd->x);
		input_report_abs(rdev, ABS_MT_POSITION_Y, cmd->y);
		input_report_key(rdev, BTN_TOUCH, 1);
		input_mt_sync(rdev);
		input_sync(rdev);
		break;
	case CMD_UP:
		input_report_abs(rdev, ABS_MT_SLOT, 0);
		input_report_abs(rdev, ABS_MT_TRACKING_ID, -1);
		input_report_key(rdev, BTN_TOUCH, 0);
		input_mt_sync(rdev);
		input_sync(rdev);
		break;
	case CMD_MT_DOWN:
		input_report_abs(rdev, ABS_MT_SLOT, cmd->param);
		input_report_abs(rdev, ABS_MT_TRACKING_ID, cmd->param + 1);
		input_report_abs(rdev, ABS_MT_POSITION_X, cmd->x);
		input_report_abs(rdev, ABS_MT_POSITION_Y, cmd->y);
		break;
	case CMD_MT_MOVE:
		input_report_abs(rdev, ABS_MT_SLOT, cmd->param);
		input_report_abs(rdev, ABS_MT_POSITION_X, cmd->x);
		input_report_abs(rdev, ABS_MT_POSITION_Y, cmd->y);
		break;
	case CMD_MT_UP:
		input_report_abs(rdev, ABS_MT_SLOT, cmd->param);
		input_report_abs(rdev, ABS_MT_TRACKING_ID, -1);
		break;
	case CMD_MT_SYNC:
		input_mt_sync_frame(rdev);
		input_sync(rdev);
		break;
	default:
		break;
	}

	ctx.inject_to_real = false;
	ctx.stats.injected++;
}

static void execute_touch_cmd(struct touch_cmd *cmd)
{
	/* 双通道注入：同时写 vdev 和真实设备 */
	inject_to_vdev(cmd);
	inject_to_real(cmd);
}

static ssize_t proc_write(struct file *file, const char __user *buf,
			  size_t count, loff_t *ppos)
{
	struct touch_cmd cmd;

	if (count < sizeof(cmd))
		return -EINVAL;

	if (copy_from_user(&cmd, buf, sizeof(cmd)))
		return -EFAULT;

	execute_touch_cmd(&cmd);
	return count;
}

static ssize_t proc_read(struct file *file, char __user *buf,
			 size_t count, loff_t *ppos)
{
	char info[512];
	int len;

	if (*ppos > 0)
		return 0;

	len = snprintf(info, sizeof(info),
		"target: %s\n"
		"intercepting: %s\n"
		"real_handle: %p\n"
		"real_dev: %p\n"
		"vdev: %p\n"
		"inject_to_real: %s\n"
		"intercepted: %llu\n"
		"injected: %llu\n",
		target_device,
		ctx.intercepting ? "yes" : "no",
		ctx.real_handle,
		ctx.real_dev,
		ctx.vdev,
		ctx.inject_to_real ? "yes" : "no",
		ctx.stats.intercepted,
		ctx.stats.injected);

	if (len > count)
		len = count;

	if (copy_to_user(buf, info, len))
		return -EFAULT;

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

	pr_info(MODULE_TAG ": loading v2, target='%s'\n", target_device);

	spin_lock_init(&ctx.event_lock);
	ctx.intercepting = false;
	ctx.inject_to_real = false;

	/* 注册 kprobe — hook input_handle_event */
	ret = register_kprobe(&kp_input_handle_event);
	if (ret) {
		pr_warn(MODULE_TAG ": kprobe register failed: %d (filter-only mode)\n", ret);
		/* 非致命，继续加载（只有 filter 模式） */
	} else {
		pr_info(MODULE_TAG ": kprobe registered on input_handle_event\n");
	}

	/* 创建 proc 接口 */
	ctx.proc_entry = proc_create(PROC_NAME, 0666, NULL, &proc_fops);
	if (!ctx.proc_entry) {
		pr_err(MODULE_TAG ": failed to create /proc/%s\n", PROC_NAME);
		unregister_kprobe(&kp_input_handle_event);
		return -ENOMEM;
	}

	/* 注册 input handler */
	ret = input_register_handler(&interceptor_handler);
	if (ret) {
		pr_err(MODULE_TAG ": failed to register handler: %d\n", ret);
		proc_remove(ctx.proc_entry);
		unregister_kprobe(&kp_input_handle_event);
		return ret;
	}

	pr_info(MODULE_TAG ": loaded successfully (dual injection mode)\n");
	return 0;
}

static void __exit touch_interceptor_exit(void)
{
	pr_info(MODULE_TAG ": unloading\n");

	input_unregister_handler(&interceptor_handler);
	proc_remove(ctx.proc_entry);
	unregister_kprobe(&kp_input_handle_event);

	pr_info(MODULE_TAG ": unloaded, intercepted=%llu injected=%llu\n",
		ctx.stats.intercepted, ctx.stats.injected);
}

module_init(touch_interceptor_init);
module_exit(touch_interceptor_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Unified touch interceptor — real + remote through one device (v2)");
MODULE_AUTHOR("interceptor");
