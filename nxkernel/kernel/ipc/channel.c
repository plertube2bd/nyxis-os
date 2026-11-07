/*
 * channel.c - svc:/ 채널 구현 (설계는 channel.h 참고)
 */

#include "kernel/ipc/channel.h"
#include "kernel/process/process.h"
#include "kernel/mm/kheap.h"
#include "lowlevel.h"
#include "memory.h"
#include "string.h"

#define CHAN_REGISTRY_MAX 32U

static channel_t *g_registry[CHAN_REGISTRY_MAX];

static bool valid_component(const char *s, usize n)
{
    usize i;

    if (n == 0 || n > 31)
        return false;
    for (i = 0; i < n; i++) {
        char c = s[i];

        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.'))
            return false;
    }
    return true;
}

static channel_t *registry_find(const char *name)
{
    u32 i;

    for (i = 0; i < CHAN_REGISTRY_MAX; i++) {
        if (g_registry[i] && strcmp(g_registry[i]->name, name) == 0)
            return g_registry[i];
    }
    return nNULL;
}

Nstatus channel_create(const char *name, u32 flags, channel_t **out)
{
    const char *slash;
    process_t *self = current_process;
    channel_t *c;
    u64 irq;
    u32 i;
    usize owner_len;

    if (!name || !out || !self)
        return NinvalidArg;
    if ((flags & ~(u32)NX_CHAN_PUBLIC) != 0)
        return NinvalidArg;

    /* 정확히 "<영역>/<이름>" 두 구성요소 */
    slash = strchr(name, '/');
    if (!slash || strchr(slash + 1, '/'))
        return NinvalidArg;
    owner_len = (usize)(slash - name);
    if (!valid_component(name, owner_len) || !valid_component(slash + 1, strlen(slash + 1)))
        return NinvalidArg;

    if (owner_len == 3 && strncmp(name, "sys", 3) == 0) {
        if (self->uid != 0)
            return Npermission;            /* 시스템 서비스 이름은 root 만 */
    } else {
        if (strlen(self->user_name) != owner_len || strncmp(name, self->user_name, owner_len) != 0)
            return Npermission;            /* 남의 이름 아래에는 만들 수 없다 */
    }

    c = (channel_t *)kzalloc(sizeof(*c));
    if (!c)
        return NoutOfMemory;
    memcpy(c->name, name, strlen(name) + 1U);
    c->owner_uid = self->uid;
    c->flags = flags;
    c->refs = 1;
    c->receiver_alive = true;
    waitq_init(&c->recv_q);
    waitq_init(&c->send_q);

    irq = irq_save();
    if (registry_find(name)) {
        irq_restore(irq);
        kfree(c);
        return NalreadyExists;
    }
    for (i = 0; i < CHAN_REGISTRY_MAX; i++) {
        if (!g_registry[i]) {
            g_registry[i] = c;
            break;
        }
    }
    irq_restore(irq);
    if (i == CHAN_REGISTRY_MAX) {
        kfree(c);
        return NoutOfMemory;
    }

    *out = c;
    return NSTATUS_OK;
}

Nstatus channel_connect(const char *name, channel_t **out)
{
    process_t *self = current_process;
    channel_t *c;
    u64 irq;

    if (!name || !out || !self)
        return NinvalidArg;

    irq = irq_save();
    c = registry_find(name);
    if (!c) {
        irq_restore(irq);
        return NnotFound;
    }
    if (!(c->flags & NX_CHAN_PUBLIC) && self->uid != 0 && self->uid != c->owner_uid) {
        irq_restore(irq);
        return Npermission;
    }
    c->refs++;
    irq_restore(irq);

    *out = c;
    return NSTATUS_OK;
}

void channel_retain(channel_t *c)
{
    u64 irq = irq_save();

    c->refs++;
    irq_restore(irq);
}

static void drop_messages_locked(channel_t *c)
{
    while (c->head) {
        chan_msg_t *m = c->head;

        c->head = m->next;
        kfree(m);
    }
    c->tail = nNULL;
    c->count = 0;
}

void channel_release(channel_t *c, bool receiver)
{
    u64 irq = irq_save();
    u32 i;

    if (receiver && c->receiver_alive) {
        c->receiver_alive = false;
        for (i = 0; i < CHAN_REGISTRY_MAX; i++) {
            if (g_registry[i] == c)
                g_registry[i] = nNULL;     /* 이름은 즉시 사라진다 (같은 이름으로 다시 만들 수 있다) */
        }
        drop_messages_locked(c);
        (void)waitq_wake_all(&c->send_q);  /* 보내려고 기다리던 쪽은 HUP 을 보게 된다 */
        (void)waitq_wake_all(&c->recv_q);
    }

    if (c->refs > 0)
        c->refs--;
    if (c->refs == 0) {
        drop_messages_locked(c);
        kfree(c);
    }
    irq_restore(irq);
}

Nstatus channel_send(channel_t *c, const void *buf, usize len)
{
    process_t *self = current_process;
    chan_msg_t *m;
    u64 irq;

    if (len > NX_CHAN_MSG_MAX)
        return NinvalidArg;

    m = (chan_msg_t *)kmalloc(sizeof(*m));
    if (!m)
        return NoutOfMemory;
    m->next = nNULL;
    m->pid = self ? self->pid : 0;
    m->uid = self ? self->uid : 0;
    m->gid = self ? self->gid : 0;
    m->len = (u32)len;
    if (len)
        memcpy(m->data, buf, len);

    irq = irq_save();
    if (!c->receiver_alive) {
        irq_restore(irq);
        kfree(m);
        return Ndisconnected;
    }
    if (c->count >= NX_CHAN_QUEUE_MAX) {
        irq_restore(irq);
        kfree(m);
        return Nbusy;
    }
    if (c->tail)
        c->tail->next = m;
    else
        c->head = m;
    c->tail = m;
    c->count++;
    irq_restore(irq);

    (void)waitq_wake_all(&c->recv_q);
    return NSTATUS_OK;
}

Nstatus channel_take_locked(channel_t *c, void *buf, usize len, struct nx_msginfo *info)
{
    chan_msg_t *m = c->head;

    if (!m)
        return Ntimeout;

    info->sender_pid = m->pid;
    info->sender_uid = m->uid;
    info->sender_gid = m->gid;
    info->length = m->len;
    if (m->len > len)
        return NbufferOverflow;

    if (m->len)
        memcpy(buf, m->data, m->len);
    c->head = m->next;
    if (!c->head)
        c->tail = nNULL;
    c->count--;
    kfree(m);

    (void)waitq_wake_all(&c->send_q);   /* 자리가 났다 */
    return NSTATUS_OK;
}

u32 channel_poll_bits(const channel_t *c, bool receiver)
{
    u32 bits = 0;

    if (receiver) {
        if (c->count > 0)
            bits |= NX_POLL_IN;
    } else {
        if (!c->receiver_alive)
            bits |= NX_POLL_HUP;
        else if (c->count < NX_CHAN_QUEUE_MAX)
            bits |= NX_POLL_OUT;
    }
    return bits;
}
