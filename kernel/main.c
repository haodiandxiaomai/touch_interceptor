/*
 * touch_interceptor — 统一触摸拦截模块
 *
 * 拦截真实触摸事件，重定向到虚拟设备。
 * 远程触摸也通过同一虚拟设备输出。
 * 上层只看到一个事件源，无法区分真实/虚拟。
 *
 * 架构：
 *   物理驱动 → [filter 拦截] → vdev → 用户空间
 *   远程控制 → [注入接口] → vdev ↗
 */

#include <linux/module.h>
#include <linux/input.h>
#include <linux/input/mt.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/proc_fs.h>
#include <linux/kallsyms.h>

#include "intercept.h"

#define MODULE_TAG "touch_interceptor"

/* ============ 配置 ============ */
static char target_device[128] = TARGET_DEVICE_NAME;
module_param_string(target, target_device, sizeof(target_device), 0644);
MODULE_PARM_DESC(target, "Name of the touchscreen device to intercept");

/* ============ 全局状态 ============ */
static struct interceptor_ctx ctx;

/* ============ 虚拟设备创建 ============ */

/*
 * 根据真实设备的能力创建虚拟设备
 * 必须在 connect 时调用，此时 dev 的能力已经设置好
 */
static int create_virtual_device(struct input_dev *real_dev)
{
    struct input_dev *vdev;
    int slot_max;
    int x_max, y_max;
    int ret;

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
    __set_bit(EV_ABS, vdev->evbit);
    __set_bit(EV_KEY, vdev->evbit);
    __set_bit(EV_SYN, vdev->evbit);

    /* 复制按键 */
    if (test_bit(BTN_TOUCH, real_dev->keybit))
        __set_bit(BTN_TOUCH, vdev->keybit);
    if (test_bit(KEY_POWER, real_dev->keybit))
        __set_bit(KEY_POWER, vdev->keybit);
    if (test_bit(KEY_WAKEUP, real_dev->keybit))
        __set_bit(KEY_WAKEUP, vdev->keybit);

    /* 复制 ABS 能力 */
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

    /* INPUT_PROP_DIRECT */
    if (test_bit(INPUT_PROP_DIRECT, real_dev->propbit))
        __set_bit(INPUT_PROP_DIRECT, vdev->propbit);

    ret = input_register_device(vdev);
    if (ret) {
        input_free_device(vdev);
        return ret;
    }

    ctx.vdev = vdev;
    pr_info(MODULE_TAG ": virtual device created: %s\n", vdev->name);
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

/*
 * filter() — 每个事件都会经过这里
 *
 * 在 input_pass_values() 之前调用，
 * 返回 true = 阻止事件传播到原始 handler (evdev 等)
 *
 * 注意：在 event_lock spinlock 下运行，不能睡眠
 */
static bool interceptor_filter(struct input_handle *handle,
                unsigned int type, unsigned int code, int value)
{
    unsigned long flags;

    if (handle != ctx.real_handle)
        return false;  /* 不拦截其他设备 */

    if (!ctx.vdev)
        return false;

    if (!ctx.intercepting)
        return false;

    /* 转发到虚拟设备 */
    spin_lock_irqsave(&ctx.event_lock, flags);
    input_event(ctx.vdev, type, code, value);
    spin_unlock_irqrestore(&ctx.event_lock, flags);

    ctx.stats.intercepted++;
    return true;  /* 阻止原始事件 */
}

/*
 * connect — 当匹配的 input_dev 出现时调用
 * （注册 handler 时会扫描已有设备）
 */
static int interceptor_connect(struct input_handler *handler,
                struct input_dev *dev,
                const struct input_device_id *id)
{
    struct input_handle *handle;
    int ret;

    /* 只连接目标设备 */
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

    /* 创建虚拟设备 */
    ret = create_virtual_device(dev);
    if (ret) {
        pr_err(MODULE_TAG ": failed to create virtual device: %d\n", ret);
        input_close_device(handle);
        input_unregister_handle(handle);
        kfree(handle);
        ctx.real_handle = NULL;
        ctx.real_dev = NULL;
        return ret;
    }

    /* 开始拦截 */
    ctx.intercepting = true;

    pr_info(MODULE_TAG ": interception active\n");
    return 0;
}

static void interceptor_disconnect(struct input_handle *handle)
{
    pr_info(MODULE_TAG ": device disconnected\n");

    ctx.intercepting = false;
    destroy_virtual_device();
    ctx.real_handle = NULL;
    ctx.real_dev = NULL;

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

/*
 * 协议：写入 16 字节的触摸命令
 *
 * [0:3]  uint32 cmd     — CMD_DOWN=0, CMD_MOVE=1, CMD_UP=2, CMD_MT_*=10+
 * [4:7]  int32  x
 * [8:11] int32  y
 * [12:15]uint32 param   — finger_id / slot / pressure
 */

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

static void execute_touch_cmd(struct touch_cmd *cmd)
{
    unsigned long flags;
    struct input_dev *vdev = ctx.vdev;

    if (!vdev || !ctx.intercepting)
        return;

    spin_lock_irqsave(&ctx.event_lock, flags);

    switch (cmd->cmd) {
    case CMD_DOWN:
        input_report_key(vdev, BTN_TOUCH, 1);
        input_report_abs(vdev, ABS_MT_POSITION_X, cmd->x);
        input_report_abs(vdev, ABS_MT_POSITION_Y, cmd->y);
        input_report_abs(vdev, ABS_MT_SLOT, 0);
        input_report_abs(vdev, ABS_MT_TRACKING_ID, 1);
        input_mt_sync(vdev);
        input_sync(vdev);
        break;

    case CMD_MOVE:
        input_report_abs(vdev, ABS_MT_POSITION_X, cmd->x);
        input_report_abs(vdev, ABS_MT_POSITION_Y, cmd->y);
        input_report_abs(vdev, ABS_MT_SLOT, 0);
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

    ctx.stats.injected++;
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
    char info[256];
    int len;

    if (*ppos > 0)
        return 0;

    len = snprintf(info, sizeof(info),
        "target: %s\n"
        "intercepting: %s\n"
        "real_handle: %p\n"
        "vdev: %p\n"
        "intercepted: %llu\n"
        "injected: %llu\n",
        target_device,
        ctx.intercepting ? "yes" : "no",
        ctx.real_handle,
        ctx.vdev,
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

    pr_info(MODULE_TAG ": loading, target='%s'\n", target_device);

    spin_lock_init(&ctx.event_lock);
    ctx.intercepting = false;

    /* 创建 proc 接口 */
    ctx.proc_entry = proc_create(PROC_NAME, 0666, NULL, &proc_fops);
    if (!ctx.proc_entry) {
        pr_err(MODULE_TAG ": failed to create /proc/%s\n", PROC_NAME);
        return -ENOMEM;
    }

    /* 注册 input handler — 会自动扫描已有设备 */
    ret = input_register_handler(&interceptor_handler);
    if (ret) {
        pr_err(MODULE_TAG ": failed to register handler: %d\n", ret);
        proc_remove(ctx.proc_entry);
        return ret;
    }

    pr_info(MODULE_TAG ": loaded successfully\n");
    return 0;
}

static void __exit touch_interceptor_exit(void)
{
    pr_info(MODULE_TAG ": unloading\n");

    input_unregister_handler(&interceptor_handler);
    proc_remove(ctx.proc_entry);

    /* disconnect 会清理 vdev 和 handle */
    pr_info(MODULE_TAG ": unloaded, intercepted=%llu injected=%llu\n",
        ctx.stats.intercepted, ctx.stats.injected);
}

module_init(touch_interceptor_init);
module_exit(touch_interceptor_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Unified touch interceptor — real + remote through one device");
MODULE_AUTHOR("interceptor");
