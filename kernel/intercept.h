/*
 * intercept.h — 触摸拦截模块定义
 */

#ifndef _TOUCH_INTERCEPTOR_H
#define _TOUCH_INTERCEPTOR_H

#include <linux/input.h>
#include <linux/spinlock.h>
#include <linux/proc_fs.h>
#include <linux/types.h>

#define PROC_NAME           "touch_interceptor"
#define VIRTUAL_DEVICE_NAME "Virtual Touchscreen"
#define TARGET_DEVICE_NAME  "NVTCapacitiveTouchScreen"

/* 统计信息 */
struct interceptor_stats {
    u64 intercepted;  /* 拦截的真实事件数 */
    u64 injected;     /* 注入的远程事件数 */
};

/* 模块上下文 */
struct interceptor_ctx {
    /* input handler */
    struct input_handler handler;
    struct input_handle *real_handle;
    struct input_dev *real_dev;

    /* 虚拟设备 */
    struct input_dev *vdev;

    /* 拦截开关 */
    bool intercepting;

    /* 事件锁 — filter 和注入都需要 */
    spinlock_t event_lock;

    /* proc 接口 */
    struct proc_dir_entry *proc_entry;

    /* 统计 */
    struct interceptor_stats stats;
};

#endif /* _TOUCH_INTERCEPTOR_H */
