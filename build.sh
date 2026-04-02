#!/bin/bash
#
# build.sh — 编译 touch_interceptor 模块
#
# 用法：
#   ./build.sh [KDIR=/path/to/kernel/source]
#
# 环境变量：
#   KDIR          内核源码路径（必须）
#   ARCH          目标架构，默认 arm64
#   CROSS_COMPILE 交叉编译前缀，默认 aarch64-linux-gnu-
#   CLANG         clang 路径（可选，Android GKI 通常需要）
#
# 示例：
#   KDIR=/path/to/kernel-6.1 ./build.sh
#   KDIR=/path/to/kernel CLANG=/path/to/clang ./build.sh
#

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
KERNEL_DIR="${KDIR:-}"
ARCH="${ARCH:-arm64}"
CROSS_COMPILE="${CROSS_COMPILE:-aarch64-linux-gnu-}"

if [ -z "$KERNEL_DIR" ]; then
    echo "错误: 必须设置 KDIR 环境变量"
    echo "用法: KDIR=/path/to/kernel ./build.sh"
    exit 1
fi

if [ ! -d "$KERNEL_DIR" ]; then
    echo "错误: KDIR 目录不存在: $KERNEL_DIR"
    exit 1
fi

if [ ! -f "$KERNEL_DIR/Makefile" ]; then
    echo "错误: KDIR 中没有找到 Makefile: $KERNEL_DIR/Makefile"
    exit 1
fi

echo "=== touch_interceptor 编译 ==="
echo "KDIR:          $KERNEL_DIR"
echo "ARCH:          $ARCH"
echo "CROSS_COMPILE: $CROSS_COMPILE"

# 检查是否有 Module.symvers，没有则先 modules_prepare
if [ ! -f "$KERNEL_DIR/Module.symvers" ]; then
    echo ""
    echo ">>> 未找到 Module.symvers，执行 modules_prepare..."
    make -C "$KERNEL_DIR" M="" modules_prepare ARCH=$ARCH CROSS_COMPILE=$CROSS_COMPILE
fi

# 清理旧产物
echo ""
echo ">>> 清理旧编译产物..."
make -C "$SCRIPT_DIR" clean KDIR="$KERNEL_DIR" ARCH=$ARCH CROSS_COMPILE=$CROSS_COMPILE

# 编译
echo ""
echo ">>> 开始编译..."
make -C "$SCRIPT_DIR" KDIR="$KERNEL_DIR" ARCH=$ARCH CROSS_COMPILE=$CROSS_COMPILE

# 检查结果
KO="$SCRIPT_DIR/touch_interceptor.ko"
if [ -f "$KO" ]; then
    echo ""
    echo "=== 编译成功 ==="
    echo "输出: $KO"
    ls -la "$KO"
    file "$KO"
else
    echo ""
    echo "=== 编译失败：未生成 .ko 文件 ==="
    exit 1
fi
