/*
 * channel.h - svc:/ 이름공간의 채널 (프로세스 간 메시지, 규칙은 include/nyx_abi.h 의 "채널" 참고)
 *
 * 이름 등록 규칙 (사칭 방지):
 *   "sys/<이름>"          uid 0 만 만들 수 있다 (init 은 svc:/sys/init)
 *   "<사용자이름>/<이름>"  그 사용자(신원의 user_name)만 만들 수 있다
 * 받는 쪽(만든 쪽)은 수신 핸들 하나만 가진다. 보내는 쪽은 NxOpen 으로 송신 핸들을 얻는다
 * (NX_CHAN_PUBLIC 이 아니면 같은 uid 또는 root 만).
 * 메시지마다 커널이 보낸 프로세스의 pid/uid/gid 를 붙인다 — 받는 쪽은 그것으로 정책을 정한다
 * (보내는 쪽이 자기 신원을 속일 방법은 없다).
 *
 * 수명: 채널 객체는 참조 카운트(수신 핸들 + 송신 핸들들)로 관리한다. 수신 핸들이 닫히면 이름이 즉시
 * 사라지고, 쌓인 메시지는 버려지며, 송신은 Ndisconnected 가 된다 (NxPoll 은 NX_POLL_HUP).
 * 메모리: 채널과 메시지는 커널 힙(kmalloc). 큐는 NX_CHAN_QUEUE_MAX 개로 제한한다 (가득 차면 Nbusy).
 */
#ifndef KERNEL_IPC_CHANNEL_H
#define KERNEL_IPC_CHANNEL_H

#include "nyxis.h"
#include "nyx_abi.h"
#include "kernel/process/waitq.h"

#define CHAN_NAME_MAX 64U

typedef struct chan_msg {
    struct chan_msg *next;
    u32 pid;
    u32 uid;
    u32 gid;
    u32 len;
    u8  data[NX_CHAN_MSG_MAX];
} chan_msg_t;

typedef struct channel {
    char name[CHAN_NAME_MAX];
    u32  owner_uid;
    u32  flags;                 /* NX_CHAN_PUBLIC */
    u32  refs;                  /* 수신 핸들 + 송신 핸들들 */
    bool receiver_alive;
    chan_msg_t *head;
    chan_msg_t *tail;
    u32  count;
    waitq_t recv_q;             /* 메시지를 기다리는 수신자 */
    waitq_t send_q;             /* 큐에 자리가 나기를 기다리는 송신자 (NxPoll OUT) */
} channel_t;

/* name("sys/init" 처럼 svc:/ 를 뗀 것)으로 채널을 만들어 등록한다. 수신 참조 1 개를 돌려준다 */
Nstatus channel_create(const char *name, u32 flags, channel_t **out);

/* 등록된 채널에 송신자로 연결한다 (참조 +1) */
Nstatus channel_connect(const char *name, channel_t **out);

/* 참조를 놓는다. receiver 이면 이름 등록을 지우고 쌓인 메시지를 버리고 모두 깨운다 */
void channel_release(channel_t *c, bool receiver);

/* 송신 참조 하나 더 (송신 핸들 복제) */
void channel_retain(channel_t *c);

/* 현재 프로세스 신원으로 메시지를 넣는다. 큐가 가득 차면 Nbusy, 수신자가 없으면 Ndisconnected */
Nstatus channel_send(channel_t *c, const void *buf, usize len);

/*
 * 메시지 하나를 꺼낸다 (인터럽트가 꺼진 상태에서 호출, 잠들지 않는다).
 * 없으면 Ntimeout(=비었음). buf 가 작으면 NbufferOverflow 이고 메시지는 남긴다 (info->length 는 채움).
 */
Nstatus channel_take_locked(channel_t *c, void *buf, usize len, struct nx_msginfo *info);

/* NxPoll 준비 상태 (NX_POLL_* 비트) */
u32 channel_poll_bits(const channel_t *c, bool receiver);

#endif /* KERNEL_IPC_CHANNEL_H */
