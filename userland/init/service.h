/*
 * service.h - init 의 서비스 관리자 (유닛). 설명은 service.c 머리말
 */
#ifndef INIT_SERVICE_H
#define INIT_SERVICE_H

#include "nyx.h"

/* 저널 우선순위 (syslog 와 같다) */
#define LOG_ERR     3
#define LOG_WARNING 4
#define LOG_NOTICE  5
#define LOG_INFO    6

/* 유닛 파일을 읽는다 (부팅 때 한 번) */
void svc_load(void);
/* boot 가 켜진 유닛을 order 순서로 시작한다 */
void svc_boot(void);

/* NxPoll 에 기다릴 프로세스 핸들을 채운다. fds[n..max) 에 넣고 넣은 수를 돌려준다. unit_of[k] = 유닛 번호 */
unsigned long svc_poll_fill(struct nx_pollfd *fds, int *unit_of, unsigned long n, unsigned long max);
/* 유닛 프로세스가 끝났다 */
void svc_exited(int unit);
/* 다음 기한(재시작, 강제 종료)까지 남은 ms. 없으면 NX_WAIT_INFINITE */
unsigned long svc_timeout_ms(void);
/* 기한이 지난 일을 한다 */
void svc_tick(void);

/* svc:/sys/init 요청 중 서비스 관리자의 것 ("nex ...", "journal-ready"). 처리했으면 1.
 * attached: 메시지에 붙어 온 핸들 (없으면 -1). 가져갔으면 *attached = -1 로 바꾼다 */
int svc_request(const char *msg, unsigned long len, const struct nx_msginfo *info, long *attached);

/* 저널에 한 줄 (journald 가 아직 없으면 조금 모아 두었다가 나중에 보낸다). unit 은 0 이어도 된다 */
void ilog(int prio, const char *unit, const char *text);
void ilog3(int prio, const char *unit, const char *a, const char *b, const char *c);

/* 핸들에 close-on-exec 를 켠다 (서비스 프로세스에 init 의 핸들이 새지 않게) */
void set_cloexec(long h);

#endif
