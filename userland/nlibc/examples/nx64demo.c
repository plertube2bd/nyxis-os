/*
 * nx64demo.c - nx64api 가장 작은 예제 (<nx64.h>). Nyxis 안에서: nlcc -o tmp:/demo.run app:/src/examples/nx64demo.c
 */
#include <nx64.h>

static void draw(void *ctx)
{
    (void)ctx;
    nx64_tui_clear(NX64_DEFAULT);
    nx64_tui_box(2, 1, 30, 5, "hello", NX64_FRAME);
    nx64_tui_text(4, 3, "q: quit", NX64_DEFAULT);
}

static int key(void *ctx, int k)
{
    (void)ctx;
    return k != 'q';
}

int main(void)
{
    struct nx64_app app = { draw, key, NULL, 0 };
    struct nx64_memory m;

    if (nx64_memory(&m) == 0 && m.ram_kib == 0)
        return 3;
    return nx64_tui_run(&app, NULL);
}
