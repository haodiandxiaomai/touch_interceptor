# touch_interceptor — 统一触摸拦截模块

> 真实触摸 + 远程触摸 → 统一虚拟设备 → 用户空间

## 核心思路

旧方案（touch_inject_hidden）：真实触摸走 slot 0-8，虚拟触摸走 slot 9-17，两套来源并存。

新方案（本项目）：
```
物理驱动 → [input_handler filter 拦截] → 虚拟设备 → 用户空间
远程控制 → [proc 注入接口] ─────────────↗
```
所有触摸事件都从同一个虚拟设备出去，检测端无法区分来源。

## 架构

```
┌─────────────┐     ┌──────────────────────┐     ┌──────────────┐
│  Goodix/NVT │     │   touch_interceptor  │     │  用户空间     │
│  触摸驱动    │────→│  input_handler.filter │────→│  evdev       │
│             │     │                      │     │  (只看到     │
└─────────────┘     │  ┌────────────────┐  │     │   虚拟设备)  │
                    │  │ Virtual Touch  │  │     └──────────────┘
┌─────────────┐     │  │ Screen (vdev)  │  │
│  远程控制    │────→│  └────────────────┘  │
│  /proc/     │     │                      │
│  touch_int..│     └──────────────────────┘
└─────────────┘
```

## 文件结构

```
touch_interceptor/
├── kernel/
│   ├── main.c           — 模块主体：handler + vdev + proc
│   ├── intercept.h      — 上下文定义
│   └── Makefile
├── remote/
│   └── remote_touch.c   — 用户空间控制工具
├── build.sh             — 编译脚本
└── README.md
```

## 编译

```bash
# 设置内核源码路径
export KDIR=/path/to/kernel-source

# 编译
./build.sh

# 或手动
make -C kernel/ KDIR=$KDIR ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu-
```

## 部署

```bash
# 推送到设备
adb push kernel/touch_interceptor.ko /data/local/tmp/
adb push remote/remote_touch /data/local/tmp/

# 加载模块
adb shell su -c "insmod /data/local/tmp/touch_interceptor.ko"

# 检查状态
adb shell su -c "cat /proc/touch_interceptor"

# 卸载
adb shell su -c "rmmod touch_interceptor"
```

## 用法

### 状态查看

```bash
./remote_touch status
```

输出：
```
target: NVTCapacitiveTouchScreen
intercepting: yes
real_handle: ffffff...
vdev: ffffff...
intercepted: 12345
injected: 67
```

### 单点触控

```bash
./remote_touch down 540 960    # 按下
./remote_touch move 540 800    # 移动
./remote_touch up              # 抬起
```

### 多点触控

```bash
./remote_touch mt down 0 540 960   # 手指 0 按下
./remote_touch mt move 0 540 800   # 手指 0 移动
./remote_touch mt down 1 300 960   # 手指 1 按下
./remote_touch mt sync             # 同步帧
./remote_touch mt up 0             # 手指 0 抬起
./remote_touch mt sync             # 同步帧
```

### 滑动

```bash
./remote_touch swipe 300 800 800 800 30 16
#                    x1  y1  x2  y2  ms steps
```

## /proc/touch_interceptor 协议

写入 16 字节二进制命令：

| 偏移 | 类型 | 字段 | 说明 |
|------|------|------|------|
| 0-3  | u32  | cmd  | 0=down 1=move 2=up 10=mt_down 11=mt_move 12=mt_up 13=mt_sync |
| 4-7  | i32  | x    | X 坐标 |
| 8-11 | i32  | y    | Y 坐标 |
| 12-15| u32  | param| finger_id / slot / 0 |

## 对比旧方案

| 维度 | touch_inject_hidden | touch_interceptor |
|------|--------------------|--------------------|
| 事件源 | 物理 + 虚拟并存 | 单一虚拟设备 |
| Slot 分配 | 物理 0-8 / 虚拟 9-17 | 统一管理 |
| 驱动依赖 | 需要知道具体驱动 | 不依赖，input 子系统层 |
| 隐藏难度 | 需要 4 维隐藏 | 天然隐藏（单一来源） |
| 代码复杂度 | 高（anti-detect/natural_touch） | 低（标准 API） |
| 编译难度 | 需要内核符号 | 标准 out-of-tree 模块 |
| 实时性 | 好 | 好（filter 在 spinlock 内） |

## 注意事项

1. **拦截后真实设备仍然存在**，但事件不会传到用户空间
2. **卸载模块会恢复原始行为**（handler disconnect 释放所有资源）
3. **filter 回调在 spinlock 下执行**，不能睡眠
4. **target 参数可配置**：`insmod touch_interceptor.ko target=YourTouchScreen`
5. **多设备支持**：如果需要拦截多个设备，修改 interceptor_ids 匹配规则

## 安全性

- 不修改内核符号地址（不用 kprobe）
- 不 hook 任何内核函数
- 标准 input 子系统 API
- 卸载完全恢复
- 不影响其他 input 设备
