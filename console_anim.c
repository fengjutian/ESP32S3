/* 控制台弹球动画：逐帧重绘 + 延时，演示 C 里动画的最小骨架
 * 编译: gcc console_anim.c -o console_anim.exe
 * 运行: console_anim.exe [帧数]
 */
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>

#define W 40
#define H 12

int main(int argc, char **argv) {
    int frames = (argc > 1) ? atoi(argv[1]) : 300;
    int x = 3, y = 3, dx = 1, dy = 1;

    for (int f = 0; f < frames; f++) {
        printf("\x1b[H");              /* 光标归位，覆盖上一帧，避免闪烁 */

        for (int r = 0; r < H; r++) {
            for (int c = 0; c < W; c++) {
                if (r == 0 || r == H - 1)      putchar('-');
                else if (c == 0 || c == W - 1) putchar('|');
                else if (r == y && c == x)     putchar('O');
                else                           putchar(' ');
            }
            putchar('\n');
        }

        x += dx; y += dy;                       /* 更新状态 */
        if (x <= 1 || x >= W - 2) dx = -dx;     /* 边界反弹 */
        if (y <= 1 || y >= H - 2) dy = -dy;

        Sleep(40);                              /* 控制帧率 ≈ 25 FPS */
    }
    return 0;
}
