/*
 * device.c - dev:/ 장치 구현 (설계는 device.h 참고)
 */

#include "kernel/dev/device.h"
#include "kernel/process/process.h"
#include "kernel/process/umem.h"
#include "kernel/paging/paging.h"
#include "console/outputs/printk.h"
#include "console/outputs/conout.h"
#include "console/outputs/serial.h"
#include "console/inputs/tty.h"
#include "console/inputs/inputs_base.h"
#include "console/outputs/outputs_base.h"
#include "console/outputs/ansi.h"
#include "kernel/syscall/uaccess.h"
#include "drivers/ramdisk/ramdisk.h"
#include "interrupt.h"
#include "phys.h"
#include "string.h"
#include "memory.h"

#define DEV_MAX        8U
#define RD0_DISKNO     2U
#define RD0_SIZE       (1024UL * 1024UL)
#define COM1_PORT      0x3F8U

static nx_device_t g_devices[DEV_MAX];
static u32 g_device_count = 0;
static u8 g_rd0[RD0_SIZE] __attribute__((aligned(4096)));
static NTBLI *g_info = nNULL;
static nx_device_t *g_fb0 = nNULL;
static nx_device_t *g_console_dev = nNULL;

static nx_device_t *add_device(const char *name, u32 kind)
{
    nx_device_t *d;

    if (g_device_count >= DEV_MAX)
        return nNULL;
    d = &g_devices[g_device_count++];
    memset(d, 0, sizeof(*d));
    d->name = name;
    d->kind = kind;
    return d;
}

void device_init(const NTBLI *info, u32 rootdisk_no, u64 rootdisk_size)
{
    nx_device_t *d;

    g_info = (NTBLI *)(usize)info;

    if (info && info->framebuffer_base && info->framebuffer_size) {
        d = add_device("fb0", NX_DEV_FRAMEBUFFER);
        if (d) {
            d->phys = virt_to_phys(info->framebuffer_base);
            d->size = info->framebuffer_size;
            d->width = info->width;
            d->height = info->height;
            d->pitch = info->pixels_per_scan_line * 4U;
            d->bpp = 32;
            d->format = info->pixel_format == NTBLI_PIXEL_RGB ? NX_FB_FORMAT_XBGR : NX_FB_FORMAT_XRGB;
            g_fb0 = d;
        }
        /* 같은 프레임버퍼 + 키보드 (콘솔 세션용) */
        if (g_fb0 && (info->pixel_format == NTBLI_PIXEL_RGB || info->pixel_format == NTBLI_PIXEL_BGR)) {
            d = add_device("console", NX_DEV_CONSOLE);
            if (d) {
                d->phys = g_fb0->phys;
                d->size = g_fb0->size;
                d->width = g_fb0->width;
                d->height = g_fb0->height;
                d->pitch = g_fb0->pitch;
                d->bpp = 32;
                d->format = g_fb0->format;
                g_console_dev = d;
            }
        }
    }

    if (serial_available()) {
        d = add_device("com1", NX_DEV_STREAM);
        if (d) {
            d->io_base = (u16)COM1_PORT;
            d->io_count = 8;
        }
    }

    if (rootdisk_size) {
        d = add_device("rootdisk", NX_DEV_BLOCK);
        if (d) {
            d->diskno = rootdisk_no;
            d->size = rootdisk_size;
            d->mounted = true;
        }
    }

    if (ramdisk_init(RD0_DISKNO, (usize)g_rd0, RD0_SIZE, g_info) == NSTATUS_OK) {
        d = add_device("rd0", NX_DEV_BLOCK);
        if (d) {
            d->diskno = RD0_DISKNO;
            d->size = RD0_SIZE;
        }
    }
}

static nx_device_t *find_device(const char *name)
{
    u32 i;

    for (i = 0; i < g_device_count; i++) {
        if (strcmp(g_devices[i].name, name) == 0)
            return &g_devices[i];
    }
    return nNULL;
}

const char *device_name_at(u32 index)
{
    return index < g_device_count ? g_devices[index].name : nNULL;
}

Nstatus device_open(const char *name, bool write, nx_device_t **out)
{
    nx_device_t *d;
    u64 flags;
    Nstatus status = NSTATUS_OK;

    if (!name || !out || !current_process)
        return NinvalidArg;
    d = find_device(name);
    if (!d)
        return NnotFound;

    flags = irq_save();
    if (d->kind == NX_DEV_BLOCK) {
        if (write) {
            if (d->mounted)
                status = Nbusy;                 /* 마운트된 파일시스템 아래를 직접 쓰면 깨진다 */
            else if (d->owner || d->readers)
                status = Nbusy;
            else
                d->owner = current_process;
        } else {
            if (d->owner)
                status = Nbusy;
            else
                d->readers++;
        }
    } else if (d->kind == NX_DEV_CONSOLE) {
        /* 콘솔 세션 자신(자식은 아니다)이나 root 만. fb0 과 배타적 */
        if (current_process->uid != 0 && !current_process->console_session) {
            status = Npermission;
        } else if (d->owner || (g_fb0 && g_fb0->owner)) {
            status = Nbusy;
        } else {
            d->owner = current_process;
            conout_set_fb_enabled(false);
            keyboard_claim(true);
        }
    } else {
        if (d->owner || (d->kind == NX_DEV_FRAMEBUFFER && g_console_dev && g_console_dev->owner)) {
            status = Nbusy;
        } else {
            d->owner = current_process;
            /* 커널이 비켜 준다 */
            if (d->kind == NX_DEV_FRAMEBUFFER) {
                conout_set_fb_enabled(false);
            } else {
                conout_set_serial_enabled(false);
                tty_set_serial_rx(false);
            }
        }
    }
    irq_restore(flags);

    if (NSTATUS_IS_ERR(status))
        return status;
    *out = d;
    return NSTATUS_OK;
}

void device_close(nx_device_t *dev, struct process *owner, bool write)
{
    u64 flags = irq_save();

    if (dev->kind == NX_DEV_BLOCK) {
        if (write) {
            if (dev->owner == owner)
                dev->owner = nNULL;
        } else if (dev->readers > 0) {
            dev->readers--;
        }
        irq_restore(flags);
        return;
    }

    if (dev->owner != owner) {
        irq_restore(flags);
        return;
    }

    if (dev->kind == NX_DEV_FRAMEBUFFER || dev->kind == NX_DEV_CONSOLE) {
        if (dev->mapped_va && owner->addrspace.pml4) {
            u64 off;

            for (off = 0; off < dev->size; off += PAGE_SIZE)
                (void)paging_unmap_page(&owner->addrspace, (void *)(usize)(dev->mapped_va + off));
        }
        dev->mapped_va = 0;
        conout_set_fb_enabled(true);
        if (dev->kind == NX_DEV_CONSOLE) {
            /* 키보드를 콘솔 터미널로 돌려주고, 세션이 그린 화면을 지운다 (다음 로그인 화면이 깨끗하게) */
            keyboard_claim(false);
            ansi_reset();
            conout_erase_display(2);
            conout_set_cursor(0, 0);
            conout_set_cursor_visible(true);
        }
    } else {
        if (owner->io_count) {
            owner->io_base = 0;
            owner->io_count = 0;
            if (owner == current_process)
                gdt_set_io_permission(0, 0);
        }
        conout_set_serial_enabled(true);
        tty_set_serial_rx(true);
    }
    dev->owner = nNULL;
    irq_restore(flags);
}

static Nstatus block_range(const nx_device_t *dev, u64 offset, usize len, usize *out_len)
{
    if ((offset % NX_BLOCK_SECTOR) != 0 || (len % NX_BLOCK_SECTOR) != 0)
        return NinvalidArg;
    if (offset >= dev->size) {
        *out_len = 0;
        return NSTATUS_OK;
    }
    if (len > dev->size - offset)
        len = (usize)(dev->size - offset);
    *out_len = len;
    return NSTATUS_OK;
}

Nstatus device_read(nx_device_t *dev, u64 *offset, void *buf, usize len, usize *got)
{
    *got = 0;

    if (dev->kind == NX_DEV_BLOCK) {
        usize n;
        Nstatus status = block_range(dev, *offset, len, &n);

        if (NSTATUS_IS_ERR(status) || n == 0)
            return status;
        status = ramdisk_read(dev->diskno, (usize)*offset, buf, n, g_info);
        if (NSTATUS_IS_ERR(status))
            return status;
        *offset += n;
        *got = n;
        return NSTATUS_OK;
    }

    if (dev->kind == NX_DEV_STREAM) {
        u8 *b = (u8 *)buf;
        usize n = 0;

        /* 블로킹하지 않는다: 지금 수신 버퍼에 있는 만큼만 */
        while (n < len && serial_rx_ready())
            b[n++] = (u8)serial_getc_nowait();
        *got = n;
        return NSTATUS_OK;
    }

    return Nunsupported;                   /* 프레임버퍼는 MMIO 로 */
}

Nstatus device_write(nx_device_t *dev, struct process *p, bool write_open, u64 *offset,
                     const void *buf, usize len, usize *done)
{
    *done = 0;

    if (dev->kind == NX_DEV_BLOCK) {
        usize n;
        Nstatus status;

        if (!write_open || dev->owner != p)
            return Npermission;
        status = block_range(dev, *offset, len, &n);
        if (NSTATUS_IS_ERR(status) || n == 0)
            return status;
        status = ramdisk_write(dev->diskno, (usize)*offset, buf, n, g_info);
        if (NSTATUS_IS_ERR(status))
            return status;
        *offset += n;
        *done = n;
        return NSTATUS_OK;
    }

    if (dev->kind == NX_DEV_STREAM) {
        const char *b = (const char *)buf;
        usize i;

        for (i = 0; i < len; i++)
            serial_putc(b[i]);              /* printk 와 달리 그대로 (개행 변환 없음) */
        *done = len;
        return NSTATUS_OK;
    }

    return Nunsupported;
}

Nstatus device_map_mmio(nx_device_t *dev, struct process *p, u64 *out_va)
{
    u64 off;
    u64 pages;

    if (dev->kind != NX_DEV_FRAMEBUFFER && dev->kind != NX_DEV_CONSOLE)
        return Nunsupported;
    if (dev->owner != p || !p->addrspace.pml4)
        return Npermission;
    if (dev->mapped_va) {
        *out_va = dev->mapped_va;
        return NSTATUS_OK;
    }

    pages = (dev->size + PAGE_SIZE - 1) / PAGE_SIZE;
    if (pages * PAGE_SIZE > NX_USER_MMIO_SIZE)
        return Noverflow;

    for (off = 0; off < pages * PAGE_SIZE; off += PAGE_SIZE) {
        /* 커널의 HHDM 매핑과 같은 캐시 속성(WB, MTRR 이 프레임버퍼를 처리)으로 맞춘다:
         * 같은 물리 주소를 서로 다른 캐시 속성으로 두 번 매핑하면 동작이 정의되지 않는다.
         * 프레임은 이 주소 공간의 소유가 아니다 (NOFREE: 주소 공간 파기 시 PFA 로 돌려주지 않음). */
        Nstatus status = paging_map_page(&p->addrspace, (void *)(usize)(dev->phys + off),
                                         (void *)(usize)(NX_USER_MMIO_BASE + off),
                                         (u64)(PAGE_USER | PAGE_RW | PAGE_NX | PAGE_NOFREE));
        if (NSTATUS_IS_ERR(status)) {
            u64 undo;

            for (undo = 0; undo < off; undo += PAGE_SIZE)
                (void)paging_unmap_page(&p->addrspace, (void *)(usize)(NX_USER_MMIO_BASE + undo));
            return status;
        }
    }

    dev->mapped_va = NX_USER_MMIO_BASE;
    *out_va = dev->mapped_va;
    return NSTATUS_OK;
}

void device_info(const nx_device_t *dev, struct nx_devinfo *out)
{
    memset(out, 0, sizeof(*out));
    out->kind = dev->kind;
    out->size = dev->size;
    out->width = dev->width;
    out->height = dev->height;
    out->pitch = dev->pitch;
    out->bpp = dev->bpp;
    out->format = dev->format;
    out->io_base = dev->io_base;
    out->io_count = dev->io_count;
}

Nstatus device_ioperm(nx_device_t *dev, struct process *p, bool on)
{
    if (dev->kind != NX_DEV_STREAM || dev->io_count == 0)
        return Nunsupported;
    if (dev->owner != p)
        return Npermission;

    p->io_base = on ? dev->io_base : 0;
    p->io_count = on ? dev->io_count : 0;
    if (p == current_process)
        gdt_set_io_permission(p->io_base, p->io_count);
    return NSTATUS_OK;
}

i64 device_console_read_user(nx_device_t *dev, struct process *p, u64 ubuf, u64 len)
{
    struct nx_keyevent ev[16];
    u64 max = len / sizeof(ev[0]);
    u64 flags;
    u32 n;

    if (dev->kind != NX_DEV_CONSOLE)
        return (i64)Nunsupported;
    if (dev->owner != p)
        return (i64)Npermission;
    if (max == 0)
        return (i64)NinvalidArg;
    if (max > sizeof(ev) / sizeof(ev[0]))
        max = sizeof(ev) / sizeof(ev[0]);

    flags = irq_save();
    while (!keyboard_events_ready()) {
        Nstatus status = waitq_sleep_locked(keyboard_waitq(), 0);

        if (NSTATUS_IS_ERR(status)) {
            irq_restore(flags);
            return (i64)status;
        }
    }
    n = keyboard_take_events(ev, (u32)max);
    irq_restore(flags);
    {
        Nstatus status = copy_to_user(ubuf, ev, (usize)n * sizeof(ev[0]));

        if (NSTATUS_IS_ERR(status))
            return (i64)status;
    }
    return (i64)((u64)n * sizeof(ev[0]));
}

Nstatus device_console_font(nx_device_t *dev, struct process *p, u64 uarg)
{
    static u8 g_font_copy[FONT_CHAR_COUNT * 16U * 16U];     /* 셀이 가장 클 때 (16x16 비트맵) */
    struct nx_fontinfo fi;
    usize need;
    Nstatus status;

    if (dev->kind != NX_DEV_CONSOLE)
        return Nunsupported;
    if (dev->owner != p)
        return Npermission;
    status = copy_from_user(&fi, uarg, sizeof(fi));
    if (NSTATUS_IS_ERR(status))
        return status;

    need = font_export(nNULL, 0);
    if (need > sizeof(g_font_copy))
        return Noverflow;
    fi.cell_w = font_cell_width();
    fi.cell_h = font_cell_height();
    fi.first = FONT_FIRST_CHAR;
    fi.count = FONT_CHAR_COUNT;
    if (fi.buf) {
        if (fi.cap < need)
            return NbufferOverflow;
        (void)font_export(g_font_copy, sizeof(g_font_copy));
        status = copy_to_user(fi.buf, g_font_copy, need);
        if (NSTATUS_IS_ERR(status))
            return status;
    }
    return copy_to_user(uarg, &fi, sizeof(fi));
}
