/*
 * remote_touch.c — 用户空间触摸控制工具
 *
 * 通过 /proc/touch_interceptor 注入触摸事件
 *
 * 用法：
 *   ./remote_touch down <x> <y>          # 单指按下
 *   ./remote_touch move <x> <y>          # 单指移动
 *   ./remote_touch up                    # 单指抬起
 *   ./remote_touch mt down <id> <x> <y>  # 多点按下
 *   ./remote_touch mt move <id> <x> <y>  # 多点移动
 *   ./remote_touch mt up <id>            # 多点抬起
 *   ./remote_touch mt sync               # 同步帧
 *   ./remote_touch swipe <x1> <y1> <x2> <y2> <ms> <steps>  # 滑动
 *   ./remote_touch status                # 查看状态
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdint.h>
#include <errno.h>
#include <math.h>
#include <time.h>

#define PROC_PATH "/proc/touch_interceptor"

#define CMD_DOWN     0
#define CMD_MOVE     1
#define CMD_UP       2
#define CMD_MT_DOWN  10
#define CMD_MT_MOVE  11
#define CMD_MT_UP    12
#define CMD_MT_SYNC  13

struct touch_cmd {
    uint32_t cmd;
    int32_t  x;
    int32_t  y;
    uint32_t param;
};

static int send_cmd(struct touch_cmd *cmd)
{
    int fd = open(PROC_PATH, O_WRONLY);
    if (fd < 0) {
        perror("open " PROC_PATH);
        return -1;
    }

    if (write(fd, cmd, sizeof(*cmd)) != sizeof(*cmd)) {
        perror("write");
        close(fd);
        return -1;
    }

    close(fd);
    return 0;
}

static void show_status(void)
{
    char buf[1024];
    int fd = open(PROC_PATH, O_RDONLY);
    if (fd < 0) {
        perror("open " PROC_PATH);
        return;
    }

    int n = read(fd, buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = '\0';
        printf("%s", buf);
    }
    close(fd);
}

/* 高斯插值 — 让滑动轨迹更自然 */
static double gaussian_interp(double t, double sigma)
{
    double x = (t - 0.5) / sigma;
    return exp(-0.5 * x * x);
}

static int do_swipe(int x1, int y1, int x2, int y2, int duration_ms, int steps)
{
    struct touch_cmd cmd;
    int i;

    if (steps < 2) steps = 2;
    if (duration_ms < 10) duration_ms = 10;

    /* 按下 */
    cmd.cmd = CMD_DOWN;
    cmd.x = x1;
    cmd.y = y1;
    cmd.param = 0;
    send_cmd(&cmd);
    usleep(16000);  /* 等一帧 */

    /* 移动 */
    for (i = 1; i <= steps; i++) {
        double t = (double)i / steps;
        /* 使用 ease-out 曲线 */
        double ease = 1.0 - (1.0 - t) * (1.0 - t);

        cmd.cmd = CMD_MOVE;
        cmd.x = x1 + (int)((x2 - x1) * ease);
        cmd.y = y1 + (int)((y2 - y1) * ease);
        cmd.param = 0;
        send_cmd(&cmd);

        usleep(duration_ms * 1000 / steps);
    }

    /* 抬起 */
    cmd.cmd = CMD_UP;
    cmd.x = 0;
    cmd.y = 0;
    cmd.param = 0;
    send_cmd(&cmd);

    return 0;
}

static int do_mt_down(int finger_id, int x, int y)
{
    struct touch_cmd cmd = {
        .cmd = CMD_MT_DOWN,
        .x = x,
        .y = y,
        .param = finger_id,
    };
    return send_cmd(&cmd);
}

static int do_mt_move(int finger_id, int x, int y)
{
    struct touch_cmd cmd = {
        .cmd = CMD_MT_MOVE,
        .x = x,
        .y = y,
        .param = finger_id,
    };
    return send_cmd(&cmd);
}

static int do_mt_up(int finger_id)
{
    struct touch_cmd cmd = {
        .cmd = CMD_MT_UP,
        .x = 0,
        .y = 0,
        .param = finger_id,
    };
    return send_cmd(&cmd);
}

static int do_mt_sync(void)
{
    struct touch_cmd cmd = {
        .cmd = CMD_MT_SYNC,
        .x = 0,
        .y = 0,
        .param = 0,
    };
    return send_cmd(&cmd);
}

static void usage(const char *prog)
{
    printf("用法:\n");
    printf("  %s down <x> <y>              单指按下\n", prog);
    printf("  %s move <x> <y>              单指移动\n", prog);
    printf("  %s up                        单指抬起\n", prog);
    printf("  %s mt down <id> <x> <y>      多点按下\n", prog);
    printf("  %s mt move <id> <x> <y>      多点移动\n", prog);
    printf("  %s mt up <id>                多点抬起\n", prog);
    printf("  %s mt sync                   同步帧\n", prog);
    printf("  %s swipe <x1> <y1> <x2> <y2> <ms> <steps>  滑动\n", prog);
    printf("  %s status                    查看状态\n", prog);
    printf("\n坐标范围: X 0~20000, Y 0~32000\n");
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "status") == 0) {
        show_status();
        return 0;
    }

    if (strcmp(argv[1], "down") == 0 && argc >= 4) {
        struct touch_cmd cmd = {
            .cmd = CMD_DOWN,
            .x = atoi(argv[2]),
            .y = atoi(argv[3]),
            .param = 0,
        };
        return send_cmd(&cmd);
    }

    if (strcmp(argv[1], "move") == 0 && argc >= 4) {
        struct touch_cmd cmd = {
            .cmd = CMD_MOVE,
            .x = atoi(argv[2]),
            .y = atoi(argv[3]),
            .param = 0,
        };
        return send_cmd(&cmd);
    }

    if (strcmp(argv[1], "up") == 0) {
        struct touch_cmd cmd = {
            .cmd = CMD_UP,
            .x = 0,
            .y = 0,
            .param = 0,
        };
        return send_cmd(&cmd);
    }

    if (strcmp(argv[1], "mt") == 0 && argc >= 3) {
        if (strcmp(argv[2], "down") == 0 && argc >= 6)
            return do_mt_down(atoi(argv[3]), atoi(argv[4]), atoi(argv[5]));
        if (strcmp(argv[2], "move") == 0 && argc >= 6)
            return do_mt_move(atoi(argv[3]), atoi(argv[4]), atoi(argv[5]));
        if (strcmp(argv[2], "up") == 0 && argc >= 4)
            return do_mt_up(atoi(argv[3]));
        if (strcmp(argv[2], "sync") == 0)
            return do_mt_sync();
    }

    if (strcmp(argv[1], "swipe") == 0 && argc >= 7) {
        return do_swipe(atoi(argv[2]), atoi(argv[3]),
                atoi(argv[4]), atoi(argv[5]),
                atoi(argv[6]), atoi(argv[7]));
    }

    usage(argv[0]);
    return 1;
}
