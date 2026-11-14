/*
 * nyx_abi.h - Nyxis 시스템 콜 ABI (커널과 유저랜드가 함께 쓰는 공개 헤더)
 *
 * 이 헤더는 "커널 내부 타입" 에 의존하지 않는다. (stdint.h 의 고정폭 정수만 사용)
 * 여기에 있는 구조체의 크기/배치는 ABI 이므로 바꾸면 안 된다. (NX_STATIC_ASSERT 는 커널 쪽에서 검사)
 *
 * 호출 규약 (int 0x80 / syscall 공통):
 *   rax = 번호, rdi, rsi, rdx, r10, r8, r9 = 인자 0~5
 *   반환 rax >= 0 성공(값의 의미는 각 호출 설명), rax < 0 오류 (Nstatus 부호 확장)
 *   syscall 명령은 rcx, r11 을 파괴한다.
 *
 * 핸들: u64. 상위 32비트 = 세대(generation), 하위 32비트 = 슬롯 번호.
 *       닫힌 핸들의 슬롯이 재사용되면 세대가 올라가므로 오래된 핸들 값은 거부된다.
 *       각 프로세스는 시작할 때 0=stdin, 1=stdout, 2=stderr 를 가진다 (첫 세대이므로 값 == 슬롯 번호).
 */
#ifndef NYX_ABI_H
#define NYX_ABI_H

#include <stdint.h>

/* ---------------- 시스템 콜 번호 ---------------- */
#define NX_SYS_GET_VERSION      2U
#define NX_SYS_GET_TIME         3U
#define NX_SYS_SLEEP            4U
#define NX_SYS_YIELD            5U
#define NX_SYS_SYSINFO          7U
#define NX_SYS_GET_IDENTITY     8U    /* NxGetIdentity(struct nx_identity *out, token) token=NX_NO_TOKEN 이면 자신 */
#define NX_SYS_POWER_OFF        9U    /* NxPowerOff() - uid 0 만. 성공하면 돌아오지 않는다 */
/*
 * NxAuthenticate(const char *name, const char *password) -> 토큰 핸들 (NX_RIGHT_SPAWN)
 *   커널이 roi:/users 의 PBKDF2-HMAC-SHA256 해시와 비교한다. 실패하면 NX_AUTH_FAIL_DELAY_MS 만큼
 *   잠든 뒤 Nauthentication (사용자 없음/비밀번호 틀림을 구분하지 않는다).
 *   토큰은 일회용이다: NxProcessCreate 의 token 인자로 쓰는 순간 소모되고 핸들도 닫힌다.
 */
#define NX_SYS_AUTHENTICATE     10U

#define NX_SYS_OPEN             32U
#define NX_SYS_CLOSE            33U
#define NX_SYS_READ             34U
#define NX_SYS_WRITE            35U
#define NX_SYS_SEEK             36U
#define NX_SYS_STAT             37U
#define NX_SYS_DUPLICATE_HANDLE 41U
/*
 * NxPoll(struct nx_pollfd *fds, n, timeout_ms) -> 준비된 항목 수 (0 = 시간 초과)
 *   n <= NX_POLL_MAX. 레벨 트리거: 조건이 참이면 즉시 돌아온다. timeout_ms: 0 = 확인만,
 *   NX_WAIT_INFINITE = 무기한. 준비 조건: 채널 수신 핸들 NX_POLL_IN(메시지 있음),
 *   채널 송신 핸들 NX_POLL_OUT(큐에 자리 있음), 상대가 사라지면 NX_POLL_HUP,
 *   프로세스 핸들 NX_POLL_EXIT(종료됨), stdin NX_POLL_IN(완성된 줄 또는 EOF).
 */
#define NX_SYS_POLL             42U
/*
 * NxDuplicateHandleEx(handle, rights_mask, flags, target_slot) -> 새 핸들 (POSIX dup/dup2 의 바탕)
 *   NX_DUP_SHARE   : 파일이면 "열린 파일" 을 공유한다 (오프셋을 함께 쓴다 - POSIX dup). 없으면 NxDuplicateHandle
 *                    처럼 새로 열어 오프셋만 복사한다. 콘솔/파이프/채널 송신 끝은 원래 공유 객체다.
 *   NX_DUP_TO_SLOT : target_slot(0 ~ NX_MAX_HANDLES-1) 번 슬롯에 만든다. 그 슬롯이 열려 있으면 먼저 닫는다 (dup2).
 *                    handle 이 이미 그 슬롯이면 아무것도 하지 않고 handle 을 돌려준다. 없으면 가장 낮은 빈 슬롯.
 *   NX_DUP_MIN_SLOT: target_slot 이상인 가장 낮은 빈 슬롯에 만든다 (fcntl F_DUPFD). NX_DUP_TO_SLOT 과 함께 쓸 수 없다.
 *   NX_DUP_CLOEXEC : 새 핸들에 NX_HFLAG_CLOEXEC 를 켠다.
 *   토큰/장치/채널 수신 끝은 복제할 수 없다 (Nunsupported). NX_RIGHT_DUP 이 필요하다.
 *
 * NxHandleFlags(handle, set, clear) -> 바뀐 뒤의 플래그. 플래그: NX_HFLAG_CLOEXEC (NxProcessExec 때 닫힌다)
 */
#define NX_SYS_DUPLICATE_HANDLE_EX 43U
#define NX_SYS_HANDLE_FLAGS     44U

/*
 * NxProcessCreate(const char *path, const char *const *argv, const char *const *envp) -> 프로세스 핸들
 *   argv/envp 는 NULL 로 끝나는 문자열 포인터 배열이며 각각 NULL 이어도 된다 (빈 배열).
 *   argv[0] 을 커널이 대신 채우지 않는다 — 관례상 호출자가 프로그램 이름을 넣는다.
 *   문자열 합계(NUL 포함)는 NX_ARG_MAX, 개수 합계는 NX_ARGV_MAX 이하 (넘으면 Noverflow).
 *   새 프로세스는 System V AMD64 초기 스택(argc, argv[], NULL, envp[], NULL, auxv{AT_NULL})으로 시작한다.
 *   반환 핸들의 권한: NX_RIGHT_WAIT | NX_RIGHT_KILL | NX_RIGHT_STAT | NX_RIGHT_DUP.
 *   pid 가 필요하면 NxProcessInfo(핸들) 로 얻는다. 새 프로세스는 부모와 완전히 독립된 주소 공간과
 *   자신만의 stdin/stdout/stderr(콘솔)를 갖는다 — 부모의 다른 핸들을 상속하지는 않는다.
 *   핸들을 닫아도 자식은 계속 실행된다 (종료 코드를 받을 수단만 사라진다). 부모가 끝나면 핸들도 닫힌다.
 *
 * NxProcessKill(handle) -> 0. NX_RIGHT_KILL 필요. 자식은 종료 코드 NX_EXIT_KILLED 로 끝난다
 *   (이미 끝났으면 아무것도 하지 않고 0).
 *
 * NxProcessWait(handle, timeout_ms, int32_t *out_code) -> 0. NX_RIGHT_WAIT 필요.
 *   프로세스가 끝날 때까지 잠든다 (CPU 를 쓰지 않는다). timeout_ms 가 NX_WAIT_INFINITE 이면 무기한,
 *   아니면 그 시간이 지나면 Ntimeout. 성공하면 *out_code 에 종료 코드 (out_code 는 NULL 이어도 된다).
 *   같은 핸들로 여러 번 기다려도 된다 (종료 코드는 핸들이 닫힐 때까지 남는다).
 *
 * NxProcessInfo(NX_PID_SELF 또는 프로세스 핸들, struct nx_procinfo *out) -> 0.
 *   핸들이면 NX_RIGHT_STAT 필요.
 */
#define NX_SYS_PROCESS_CREATE   64U
#define NX_SYS_PROCESS_EXIT     65U
#define NX_SYS_PROCESS_KILL     66U
#define NX_SYS_PROCESS_WAIT     67U
#define NX_SYS_PROCESS_INFO     68U
/*
 * NxProcessFork() -> 부모에게는 자식 프로세스 핸들(NxProcessCreate 와 같은 권한), 자식에게는 0.
 *   자식은 부모 사용자 메모리의 복사본(지금은 즉시 복사; 쓰기 시 복사는 아직 없다), 같은 레지스터,
 *   같은 FPU 상태, 같은 신원과 umask 로 시작한다. 핸들 테이블은 슬롯 번호와 세대까지 그대로 물려받는다
 *   (그래서 메모리에 저장된 핸들 값이 자식에서도 유효하다). 열린 파일은 오프셋을 공유한다.
 *   물려받지 않는 것: 프로세스 핸들(부모의 자식은 자식의 자식이 아니다), 토큰(일회용 비밀), 장치(독점),
 *   채널 수신 끝(받는 쪽은 하나). 그 슬롯은 자식에서 닫힌 상태다.
 *
 * NxProcessExec(const char *path, argv, envp) -> 성공하면 돌아오지 않는다 (새 프로그램이 시작된다).
 *   현재 프로세스(pid, 신원, umask, 부모 관계, 핸들)는 그대로 두고 프로그램 이미지만 바꾼다.
 *   새 이미지를 다 만든 뒤에 옛 이미지를 버리므로, 실패하면 옛 프로그램이 오류를 받고 계속 실행된다.
 *   NX_HFLAG_CLOEXEC 핸들은 닫힌다. 남은 핸들의 세대는 0 으로 돌아간다: 새 프로그램에서 핸들 값 = 슬롯 번호.
 *   실행 파일에는 실행 권한(x 비트)이 있어야 한다 (NxProcessCreate 도 같다).
 */
#define NX_SYS_PROCESS_FORK     73U
#define NX_SYS_PROCESS_EXEC     74U

/*
 * NxProcessGroup(NX_PID_SELF 또는 프로세스 핸들, pgid) -> 그 프로세스의 (바뀐) 프로세스 그룹 번호.
 *   프로세스 그룹 = 콘솔의 Ctrl-C 가 함께 끝내는 묶음 (셸의 "작업" 하나). 새 프로세스는 만든 프로세스의 그룹을
 *   물려받는다 (커널이 만든 init 은 자기 pid). 핸들이면 NX_RIGHT_KILL 필요 (자기 자식만 옮길 수 있다).
 *   pgid: 0 = 대상의 pid 로 새 그룹, NX_PGID_QUERY = 바꾸지 않고 조회만, 그 밖 = 그 그룹으로 옮긴다.
 */
#define NX_SYS_PROCESS_GROUP    75U
#define NX_PGID_QUERY           0xFFFFFFFFUL

/*
 * 신호 (kernel/process/signal.h). 번호 1..31 (NX_SIG*), 집합(mask)은 비트 (sig - 1).
 *
 * NxSignalAction(sig, const struct nx_sigaction *act 또는 NULL, struct nx_sigaction *old 또는 NULL) -> 0.
 *   handler: NX_SIG_DFL(기본 동작) / NX_SIG_IGN(무시) / 사용자 함수 주소. 함수면 restorer(처리기가 돌아갈 곳,
 *   NxSignalReturn 을 부르는 코드)가 꼭 있어야 한다. SIGKILL 과 SIGSTOP 은 바꿀 수 없다 (NinvalidArg).
 *   무시로 바꾸면 그 신호의 대기 중인 것은 버린다. fork 는 물려주고, exec 는 함수 처리기만 기본으로 되돌린다.
 * NxSignalMask(how, set) -> 바꾸기 전의 막은 집합. how: NX_SIG_BLOCK / UNBLOCK / SETMASK. SIGKILL/SIGSTOP 은 막히지 않는다.
 * NxSignalSend(NX_PID_SELF 또는 프로세스 핸들, sig) -> 0. 핸들이면 NX_RIGHT_KILL. sig 0 은 확인만.
 *   SIGKILL 은 NxProcessKill 과 같다 (바로 종료, 종료 코드 NX_EXIT_KILLED).
 * NxSignalReturn() -> 처리기에서 돌아온다 (restorer 가 부른다). 사용자 스택의 struct nx_sigcontext 로 처리기 직전의
 *   레지스터/FPU/막은 집합을 되돌린다 (rip/rsp 는 사용자 영역, rflags 는 사용자 비트만 받는다). 돌아오지 않는다.
 * NxSignalSuspend(set) -> 막은 집합을 잠시 set 으로 바꾸고 신호가 올 때까지 잔다. 늘 Ninterrupted.
 *   처리기가 끝나면 원래 집합으로 돌아간다 (sigsuspend/pause).
 * NxSignalPending() -> 막혀서 대기 중인 신호 집합.
 *
 * 전달: 사용자 모드로 돌아가기 직전(시스템 콜, 인터럽트, 예외)에 막히지 않은 대기 신호를 하나씩 처리한다.
 *   기본 동작은 무시(SIGCHLD SIGCONT SIGURG SIGWINCH, 그리고 멈춤을 지원하지 않아 SIGTSTP SIGTTIN SIGTTOU)
 *   또는 종료(종료 코드 NX_EXIT_SIGNAL(sig)). 처리기는 사용자 스택(red zone 128 바이트 아래)에 struct nx_sigframe 을
 *   쌓고 handler(sig, struct nx_siginfo *, struct nx_sigcontext *) 로 불린다. 처리 중에는 그 신호와 act.mask 가 막힌다
 *   (NX_SA_NODEFER 면 그 신호는 빼고, NX_SA_RESETHAND 면 처리기를 기본으로 되돌린다).
 *   잠든 시스템 콜(읽기, 대기, NxSleep, NxPoll, 파이프/채널)은 깨어나 Ninterrupted 를 돌려준다 (다시 시작하지 않는다).
 * 보내는 곳: NxSignalSend, 콘솔(Ctrl-C = SIGINT, Ctrl-\ = SIGQUIT -> 포그라운드 그룹), CPU 예외(처리기가 있고 막히지 않았을
 *   때만: #PF #GP #SS #AC -> SIGSEGV, #DE #MF #XM -> SIGFPE, #UD -> SIGILL, #DB #BP -> SIGTRAP. 아니면 NX_EXIT_FAULT 로 종료),
 *   자식의 종료(부모에게 SIGCHLD).
 */
#define NX_SYS_SIGNAL_ACTION    76U
#define NX_SYS_SIGNAL_MASK      77U
#define NX_SYS_SIGNAL_SEND      78U
#define NX_SYS_SIGNAL_RETURN    79U
#define NX_SYS_SIGNAL_SUSPEND   80U
#define NX_SYS_SIGNAL_PENDING   81U
/*
 * NxProcessList(struct nx_procent *out, max) -> 살아 있는 프로세스 수 (max 보다 크면 앞의 max 개만 채운다).
 *   누구나 부를 수 있다 (이름/소유자/상태/메모리/CPU 틱만: 인자, 환경, 핸들 내용은 보이지 않는다). ntop 이 쓴다.
 * NxSignalSendPid(pid, sig) -> 0. 핸들 없이 pid 로 신호를 보낸다 (NxSignalSend 와 같은 뜻, sig 0 은 확인만).
 *   root 는 누구에게나, 그 밖에는 같은 uid 의 사용자 프로세스에게만 (아니면 Npermission). 커널 스레드(idle 등)는
 *   Npermission, 없는 pid 는 NnotFound.
 */
#define NX_SYS_PROCESS_LIST     82U
#define NX_SYS_SIGNAL_SEND_PID  83U

#define NX_SIG_MAX      32U          /* 신호 번호 < NX_SIG_MAX */
#define NX_SIGHUP       1U
#define NX_SIGINT       2U
#define NX_SIGQUIT      3U
#define NX_SIGILL       4U
#define NX_SIGTRAP      5U
#define NX_SIGABRT      6U
#define NX_SIGBUS       7U
#define NX_SIGFPE       8U
#define NX_SIGKILL      9U
#define NX_SIGUSR1      10U
#define NX_SIGSEGV      11U
#define NX_SIGUSR2      12U
#define NX_SIGPIPE      13U
#define NX_SIGALRM      14U
#define NX_SIGTERM      15U
#define NX_SIGCHLD      17U
#define NX_SIGCONT      18U
#define NX_SIGSTOP      19U
#define NX_SIGTSTP      20U
#define NX_SIGTTIN      21U
#define NX_SIGTTOU      22U
#define NX_SIGURG       23U
#define NX_SIGWINCH     28U

#define NX_SIG_DFL      0UL
#define NX_SIG_IGN      1UL

#define NX_SA_SIGINFO   0x1U         /* 정보용 (처리기는 늘 세 인자로 불린다) */
#define NX_SA_NODEFER   0x2U
#define NX_SA_RESETHAND 0x4U

#define NX_SIG_BLOCK    0UL
#define NX_SIG_UNBLOCK  1UL
#define NX_SIG_SETMASK  2UL

/* nx_siginfo.code */
#define NX_SI_USER      0            /* NxSignalSend: pid = 보낸 프로세스 */
#define NX_SI_FAULT     1            /* CPU 예외: addr = 잘못된 주소 (#PF 의 CR2) 또는 명령 주소 */
#define NX_SI_CHILD     1            /* SIGCHLD: pid = 끝난 자식 */
#define NX_SI_KERNEL    0x80         /* 콘솔(Ctrl-C 등) */

/*
 * NxVirtualAlloc(increment) -> 새 brk 가상 주소 (u64, 항상 >= 0 이므로 오류와 헷갈리지 않는다)
 *   increment 는 부호 있는 바이트 수(i64 로 해석): 양수면 힙을 늘리고, 음수면 줄이고,
 *   0 이면 아무것도 바꾸지 않고 현재 brk 를 그대로 돌려준다 (고전적인 sbrk(0) 관용구).
 *   현재는 brk(증가분) 방식만 지원한다 — 주소/길이를 직접 지정하는 mmap 스타일 할당이나
 *   보호 속성 변경은 아직 없다 (NxVirtualFree(97) 은 그 미래 API를 위해 번호만 예약해
 *   두었고, v1 에서는 구현하지 않는다: 지금은 NxVirtualAlloc 에 음수를 넘기는 것이 "해제"다).
 */
#define NX_SYS_VIRTUAL_ALLOC    96U
/*
 * 공유 메모리 (kernel/mm/shm.h). 컴포지터의 화면 버퍼처럼 프로세스 사이에 큰 데이터를 복사 없이 나눈다.
 *   NxSharedMemoryCreate(size, flags) -> 핸들 (READ|WRITE|STAT|DUP). 0 으로 채운 size 바이트(페이지 단위로 올림,
 *     NX_SHM_MAX 까지. 모든 객체 합계 NX_SHM_TOTAL_MAX). flags: NX_DUP_CLOEXEC.
 *   NxSharedMemoryMap(h, u64 *out_addr) -> 0. 객체 전체를 매핑한다 (WRITE 권한이 있으면 읽기/쓰기, 없으면 읽기만).
 *     같은 핸들로 다시 부르면 같은 주소. 매핑은 핸들에 속한다: 핸들을 닫거나 exec 하면 사라진다.
 *   다른 프로세스에게는 NxChannelSendHandle 로 핸들을 보낸다 (읽기만 주려면 NxDuplicateHandle 로 WRITE 를 빼고 보낸다).
 *   NxStat: type NX_TYPE_SHM, size = 바이트. fork 의 자식은 핸들은 물려받고 매핑은 없다.
 */
#define NX_SYS_SHM_CREATE       99U
#define NX_SYS_SHM_MAP          100U

/*
 * 채널 (svc:/ 이름공간의 IPC). 메시지마다 커널이 보낸 쪽 pid/uid/gid 를 붙여 준다.
 *   NxChannelCreate(const char *path, flags) -> 수신 핸들 (NX_RIGHT_RECV)
 *     path: "svc:/sys/<이름>" (uid 0 만) 또는 "svc:/<자기 사용자 이름>/<이름>". 이미 있으면 NalreadyExists.
 *     flags: NX_CHAN_PUBLIC 이면 누구나 보낼 수 있고, 아니면 같은 uid(와 root)만.
 *     수신 핸들을 닫으면 이름이 사라지고 보내는 쪽은 Ndisconnected 를 받는다.
 *   NxOpen("svc:/...", NX_O_WRITE) -> 송신 핸들 (NX_RIGHT_SEND)
 *   NxChannelSend(h, const void *buf, len) -> 0. 큐가 가득 차면 Nbusy (NxPoll 의 NX_POLL_OUT 로 기다림).
 *   NxChannelReceive(h, void *buf, len, struct nx_msginfo *info, timeout_ms) -> 메시지 바이트 수
 *     메시지가 없으면 잠든다(timeout_ms 규칙은 NxPoll 과 같다). buf 가 작으면 NbufferOverflow 이고
 *     메시지는 큐에 남는다 (info->length 로 필요한 크기를 알려준다).
 */
/*
 * NxPipeCreate(u64 out[2], flags) -> 0. out[0] = 읽기 끝(READ|STAT|DUP), out[1] = 쓰기 끝(WRITE|STAT|DUP).
 *   flags: NX_DUP_CLOEXEC 이면 두 핸들 모두 NX_HFLAG_CLOEXEC. 의미는 kernel/ipc/pipe.h (POSIX 파이프와 같다):
 *   빈 파이프 읽기는 잠들고 쓰는 쪽이 모두 닫히면 0(EOF), 읽는 쪽이 모두 닫힌 뒤의 쓰기는 Ndisconnected.
 *   NX_PIPE_ATOMIC 이하의 쓰기는 다른 쓰기와 섞이지 않는다. NxPoll 지원 (IN/OUT/HUP).
 */
#define NX_SYS_PIPE_CREATE      128U
#define NX_SYS_CHANNEL_CREATE   129U
#define NX_SYS_CHANNEL_SEND     130U
#define NX_SYS_CHANNEL_RECEIVE  131U

/*
 * NxPtyCreate(u64 out[2], flags) -> 0. 의사 터미널 (console/inputs/tty.h).
 *   out[0] = 주 끝(master, READ|WRITE|STAT|DUP): 쓴 바이트는 키 입력이 되고(종속 끝의 줄 편집기/raw 규칙),
 *            읽으면 종속 끝의 출력과 에코가 걸러지지 않은 그대로 나온다. NxStat 의 type 은 NX_TYPE_PTY.
 *   out[1] = 종속 끝(slave, READ|WRITE|STAT|DUP): 콘솔 핸들 (NX_TYPE_CONSOLE). 콘솔과 같은 NX_DEVCMD_TTY_* 를 받는다.
 *            새 프로세스의 0 번 핸들이 종속 끝이면 그 pty 가 그 프로세스의 제어 터미널(dev:/tty)이 된다.
 *   flags: NX_DUP_CLOEXEC 이면 두 핸들 모두 NX_HFLAG_CLOEXEC.
 *   창 크기는 주 끝(또는 종속 끝)에 NX_DEVCMD_TTY_SET_WINSIZE 로 정한다 (기본 24 x 80). 바뀌면 포그라운드 그룹에 SIGWINCH.
 *   주 끝이 모두 닫히면 끊김: 종속 끝 읽기는 0, 쓰기는 Ndisconnected, 그 터미널을 쓰는 프로세스에 SIGHUP.
 *   종속 끝이 모두 닫히면(제어 터미널로 쓰는 프로세스 포함) 주 끝 읽기는 남은 것을 다 읽은 뒤 0, NxPoll 은 HUP.
 */
#define NX_SYS_PTY_CREATE       134U
/*
 * 핸들 보내기 (Unix 소켓의 SCM_RIGHTS 와 같다: 컴포지터 연결에 쓴다).
 *   NxChannelSendHandle(h, buf, len, handle) -> 0. 메시지와 함께 handle 의 사본(같은 객체, 같은 권한)을 보낸다.
 *     handle 은 NX_RIGHT_DUP 이 있어야 하고 파이프/pty/콘솔/파일/공유 메모리/채널 송신 끝/info 만 된다
 *     (프로세스, 토큰, 장치, 채널 수신 끝은 Nunsupported). 보낸 쪽 핸들은 그대로 남는다.
 *   NxChannelReceiveEx(h, buf, len, struct nx_msginfo_ex *info, timeout_ms) -> 메시지 바이트 수.
 *     NxChannelReceive 와 같고, 핸들이 붙어 있었으면 info->handle 에 새 핸들 (없으면 NX_HANDLE_INVALID_VALUE).
 *     핸들 자리가 없으면 NoutOfMemory (메시지는 큐에 남는다). NxChannelReceive 로 받으면 붙은 핸들은 닫힌다.
 *   받지 않은 채 채널이 사라지면 붙은 핸들도 닫힌다.
 */
#define NX_SYS_CHANNEL_SEND_HANDLE 135U
#define NX_SYS_CHANNEL_RECEIVE_EX  136U

/*
 * 파일시스템 노드 (경로 규칙은 아래 "이름공간" 참고)
 *   NxCreateNode(path, type, mode)   type: NX_NODE_FILE / NX_NODE_DIR, mode: 권한 비트(0777 이하)
 *   NxRemoveNode(path)               파일 또는 빈 디렉터리
 *   NxRenameNode(old_path, new_path) 같은 볼륨 안에서만. 대상이 있으면 NalreadyExists
 *   NxReadDir(path, index, struct nx_dirent *out)  index 번째 항목. 범위를 넘으면 NnotFound
 */
#define NX_SYS_CREATE_NODE      162U
#define NX_SYS_REMOVE_NODE      163U
#define NX_SYS_RENAME_NODE      164U
#define NX_SYS_READ_DIR         165U
/*
 * 보안 계층 (파일 권한)
 *   NxStatPath(path, struct nx_stat *out)  열지 않고 정보만 (경로의 디렉터리 순회 권한만 필요). rights 는 0
 *   NxSetMode(path, mode)                  권한 비트 바꾸기 (chmod). 소유자 또는 uid 0 만. mode <= 0777
 *   NxSetUmask(mask) -> 이전 마스크         NxCreateNode 의 mode 에서 지울 비트 (기본 022). fork/생성 시 물려준다
 */
#define NX_SYS_STAT_PATH        166U
#define NX_SYS_SET_MODE         167U
#define NX_SYS_SET_UMASK        168U

/*
 * 장치 (NxOpen("dev:/...") 으로 연다. uid 0 만. 장치는 배타적으로 소유된다 - 다른 프로세스가 쓰는 중이면 Nbusy.
 * 핸들을 닫거나 프로세스가 끝나면 커널로 돌아간다)
 *   dev:/fb0      프레임버퍼. NxDeviceMapMMIO 로 자기 주소 공간에 매핑. 소유 중에는 커널 콘솔이 그리지 않는다
 *   dev:/com1     COM1 시리얼. NxRead/NxWrite(바이트, 읽기는 블로킹 안 함), NX_DEVCMD_IOPERM 으로 포트 직접 접근.
 *                 소유 중에는 커널 로그/stdin 이 COM1 을 쓰지 않는다
 *   dev:/rd0      비어 있는 RAM 블록 장치 (마운트되지 않음). NxRead/NxWrite/NxSeek, 512바이트 단위
 *   dev:/rootdisk fs:/ 가 올라간 RAM 디스크. 마운트되어 있으므로 읽기만 된다 (쓰기 열기는 Nbusy)
 * 콘솔 화면 장치 (콘솔 세션 프로세스와 root 만):
 *   dev:/console  화면(프레임버퍼)과 키보드를 함께 독점한다 (데스크톱 같은 "디스플레이 서버" 용). 열 수 있는 것은
 *                 uid 0, 또는 로그인이 NX_SPAWN_CONSOLE_SESSION 으로 띄운 그 프로세스 자신뿐이다 (자식은 안 된다:
 *                 로그아웃 뒤에 남은 프로세스가 화면/키보드를 잡아 가짜 로그인 화면을 띄우지 못하게).
 *                 dev:/fb0 과 함께 열 수 없다 (Nbusy). 프레임버퍼가 없으면 NnotFound.
 *                 - NxDeviceMapMMIO: 프레임버퍼 매핑 (NX_DEVCMD_INFO 의 width/height/pitch/format)
 *                 - NxRead: struct nx_keyevent 들 (len 은 8 의 배수, 최소 8). 없으면 잠든다. NxPoll 의 IN.
 *                   그동안 PS/2 키는 콘솔 터미널(stdin)로 가지 않는다 (시리얼 입력은 그대로 콘솔로).
 *                   Alt+SysRq 는 그대로 커널이 처리한다.
 *                 - NX_DEVCMD_CONSOLE_FONT: 커널 콘솔 글꼴 (struct nx_fontinfo)
 *                 닫거나 프로세스가 끝나면 화면을 지우고 커널 콘솔로 돌려준다.
 * 표준 스트림 장치 (누구나 연다. 독점이 아니다):
 *   dev:/tty      콘솔 터미널 자체 (리디렉션과 무관하게 화면/키보드). 쓰기는 커널 로그를 거치지 않는다
 *   dev:/stdin    호출자의 0/1/2 번 핸들이 가리키는 객체를 공유하는 새 핸들 (dup 와 같다: 파일이면 오프셋도
 *   dev:/stdout   공유). 리디렉션되어 있으면 그 파일/파이프. 그 번호가 닫혀 있으면 NnotFound,
 *   dev:/stderr   원래 핸들에 없는 읽기/쓰기 권한을 요청하면 Npermission. close-on-exec 는 이어지지 않는다
 *   NxDeviceCommand(h, cmd, arg)     NX_DEVCMD_*
 *   NxDeviceMapMMIO(h, u64 *out_addr) 장치 메모리를 매핑하고 사용자 가상 주소를 돌려준다
 */
#define NX_SYS_DEVICE_COMMAND   193U
#define NX_SYS_DEVICE_MAP_MMIO  194U

/* 디버그 구역 (0x0300..0x03FF): 커널을 make DEBUG_SYSCALLS=1 (또는 SELFTEST=1) 로 빌드했을 때만 있다.
 * 일반 커널에서는 이 구역의 모든 번호가 NsyscallFailed 다. 일반 프로그램은 쓰지 말 것 */
#define NX_SYS_DEBUG_FIRST      768U
#define NX_SYS_DEBUG_LAST       1023U
#define NX_SYS_KERNEL_PRINT     771U
#define NX_SYS_DEBUG_NOP        777U

/* ---------------- 버전 ---------------- */
/* NxGetVersion 의 반환값: (major << 32) | (minor << 16) | patch */
/* 2.0: NxProcessCreate 가 pid 대신 프로세스 핸들을 돌려주고 argv/envp 를 받는다 (호환되지 않는 변경).
 *      NxProcessKill/NxProcessWait 추가, NxRead(stdin) 이 콘솔 입력을 읽는다.
 * 2.1: 신원/인증(NxGetIdentity, NxAuthenticate, NxProcessCreate 의 token 인자), 이름공간(fs:/ app:/ face:/
 *      roi:/ tmp:/ dev:/ svc:/), 파일 노드 호출, 장치, 채널, NxPoll, NxPowerOff.
 * 2.2: info:/ 이름공간, roi:/<uid>/ 사용자 공간, NxGetIdentity 의 token 인자, NX_UID_TEMPLATE.
 * 2.3: NX_CLOCK_REALTIME, 실행 계층(NxProcessFork/Exec, NxProcessCreate 의 flags), IPC 계층(NxPipeCreate),
 *      핸들(NxDuplicateHandleEx/NxHandleFlags), 보안 계층(NxStatPath/NxSetMode/NxSetUmask, 실행 권한 검사),
 *      nx_stat 의 mode/uid/gid, nx_procinfo 의 parent_pid, 표준 스트림 장치(dev:/stdin stdout stderr tty).
 *      콘솔 쓰기는 커널 로그(printk)가 아니라 터미널 장치로 간다. dev:/ 목록(NxReadDir). NX_O_APPEND.
 * 2.4: 신호(NxSignalAction/Mask/Send/Return/Suspend/Pending, 종료 코드 NX_EXIT_SIGNAL), 프로세스 그룹
 *      (NxProcessGroup), 콘솔의 포그라운드 그룹(NX_DEVCMD_TTY_FOREGROUND, Ctrl-C = SIGINT). disk:/ 이름공간.
 * 2.5: 노드 = 하나의 오브젝트 (파일/디렉터리 구분 없음: 어느 노드든 내용과 자식을 함께 가진다. nx_stat/nx_dirent
 *      의 type 은 "지금 자식이 있으면 NX_TYPE_DIR"), NX_O_TRUNC.
 * 2.6: 의사 터미널(NxPtyCreate, NX_TYPE_PTY, NX_DEVCMD_TTY_SET_WINSIZE, 프로세스별 제어 터미널), 콘솔 화면 장치
 *      dev:/console (NX_SPAWN_CONSOLE_SESSION, struct nx_keyevent, NX_DEVCMD_CONSOLE_FONT), nx_devinfo.format,
 *      NxProcessList(struct nx_procent), NxSignalSendPid, 보호 프로세스(NX_SPAWN_CRITICAL, NX_PROCENT_CRITICAL).
 * 2.7: 공유 메모리(NxSharedMemoryCreate/Map, NX_TYPE_SHM), 핸들 보내기(NxChannelSendHandle/ReceiveEx). */
#define NX_ABI_MAJOR   2UL
#define NX_ABI_MINOR   7UL
#define NX_ABI_PATCH   0UL
#define NX_ABI_VERSION ((NX_ABI_MAJOR << 32) | (NX_ABI_MINOR << 16) | NX_ABI_PATCH)

/* ---------------- 핸들 ---------------- */
#define NX_HANDLE_STDIN   0UL
#define NX_HANDLE_STDOUT  1UL
#define NX_HANDLE_STDERR  2UL

/* 핸들에 붙는 권한 (NxDuplicateHandle 은 권한을 "줄이기만" 할 수 있다) */
#define NX_RIGHT_READ   0x01UL
#define NX_RIGHT_WRITE  0x02UL
#define NX_RIGHT_SEEK   0x04UL
#define NX_RIGHT_STAT   0x08UL
#define NX_RIGHT_DUP    0x10UL
#define NX_RIGHT_WAIT   0x20UL    /* 프로세스 핸들: NxProcessWait */
#define NX_RIGHT_KILL   0x40UL    /* 프로세스 핸들: NxProcessKill */
#define NX_RIGHT_SPAWN  0x80UL    /* 토큰 핸들: NxProcessCreate 의 신원으로 쓰기 */
#define NX_RIGHT_SEND   0x100UL   /* 채널 송신 */
#define NX_RIGHT_RECV   0x200UL   /* 채널 수신 */
#define NX_RIGHT_DEVICE 0x400UL   /* 장치: NxDeviceCommand / NxDeviceMapMMIO */
#define NX_RIGHT_ALL    0x7FFUL

/* NxDuplicateHandleEx / NxPipeCreate 플래그 */
#define NX_DUP_SHARE    0x1UL
#define NX_DUP_TO_SLOT  0x2UL
#define NX_DUP_CLOEXEC  0x4UL
#define NX_DUP_MIN_SLOT 0x8UL

/* 핸들 플래그 (NxHandleFlags) */
#define NX_HFLAG_CLOEXEC 0x1UL

/* NxProcessCreate 의 다섯 번째 인자 flags */
#define NX_SPAWN_CONSOLE_SESSION 0x2UL  /* uid 0 만: 새 프로세스가 콘솔 세션이 된다 (dev:/console 을 열 수 있다).
                                         로그인이 로그인 셸을 띄울 때 쓴다 */
#define NX_SPAWN_CRITICAL 0x4UL         /* 보호 프로세스(uid 0 이면서 자신도 보호 프로세스인 쪽만: init -> login): 부모 말고는
                                         root 도 신호/종료를 보낼 수 없다 (Npermission). 그룹 신호(Ctrl-C 등)도 가지 않는다.
                                         자기 종료, 커널의 예외 처리는 그대로. 물려주지 않는다 (fork, 자식) */
#define NX_SPAWN_INHERIT 0x1UL    /* NX_HFLAG_CLOEXEC 가 아닌 핸들을 같은 슬롯 번호로 물려준다 (세대는 0: 값 = 슬롯 번호).
                                     물려주지 않는 종류는 NxProcessFork 와 같다. 없으면 자기 콘솔 0/1/2 로 시작 */

/* ---------------- NxOpen 플래그 ---------------- */
#define NX_O_READ   0x1UL     /* 읽기 */
#define NX_O_WRITE  0x2UL     /* 쓰기 (읽기 전용 파일시스템에서는 NreadOnly) */
#define NX_O_APPEND 0x4UL     /* 파일: 쓰기마다 먼저 끝으로 옮긴다. 열린 파일의 속성이라 dup/fork/상속/exec 에도
                                 이어진다 (POSIX O_APPEND). 파일이 아닌 대상에서는 무시 */
#define NX_O_TRUNC  0x8UL     /* NX_O_WRITE 와 함께: 열면서 내용을 0 바이트로 자른다 (자식은 그대로). 파일시스템
                                 노드가 아닌 대상에서는 무시 */
#define NX_O_MASK   0xFUL

/* ---------------- NxSeek ---------------- */
#define NX_SEEK_SET 0UL
#define NX_SEEK_CUR 1UL
#define NX_SEEK_END 2UL

/* ---------------- NxGetTime ---------------- */
#define NX_CLOCK_MONOTONIC 0UL   /* 부팅 후 경과 시간(나노초). 해상도는 타이머 주기(현재 10ms) */
/*
 * NX_CLOCK_REALTIME(1): 1970-01-01 00:00:00 UTC 이후 나노초. 커널이 처음 요청받을 때 CMOS RTC 를 한 번
 * 읽어 부팅 시각을 정하고, 그 뒤로는 MONOTONIC 을 더한다 (그래서 거꾸로 가지 않는다). RTC 는 UTC 로
 * 맞춰져 있다고 가정한다. RTC 를 읽을 수 없으면 Nunsupported — libc 의 time() 은 이를 (time_t)-1 로 알린다.
 */
#define NX_CLOCK_REALTIME  1UL

/* ---------------- 제한 ---------------- */
#define NX_PATH_MAX        256UL      /* NxOpen 경로 (NUL 포함) */
#define NX_IO_MAX          0x100000UL /* 한 번의 읽기/쓰기 최대 바이트 (넘으면 잘라서 처리) */
#define NX_MAX_HANDLES     64UL       /* 프로세스당 핸들 수 (2.7: 32 -> 64, 컴포지터가 클라이언트마다 파이프 2 개 + 버퍼 1 개) */
#define NX_SLEEP_MAX_MS    3600000UL  /* NxSleep 최대 1시간 */
#define NX_PID_SELF        0xFFFFFFFFUL
#define NX_PROC_NAME_MAX   32U        /* nx_procent.name (NUL 포함) */
#define NX_ARG_MAX         4096UL     /* NxProcessCreate: argv+envp 문자열 합계 (NUL 포함) */
#define NX_ARGV_MAX        256UL      /* NxProcessCreate: argv+envp 항목 수 합계 */
#define NX_WAIT_INFINITE   0xFFFFFFFFFFFFFFFFUL  /* NxProcessWait 시간 제한 없음 */
#define NX_WAIT_MAX_MS     3600000UL  /* 그 외 timeout_ms 최대 1시간 */

/* 커널이 정하는 종료 코드 (프로그램이 NxProcessExit 로 주는 값과 겹치지 않도록 음수) */
#define NX_EXIT_FAULT      (-1)       /* 사용자 모드 CPU 예외로 종료됨 */
#define NX_EXIT_KILLED     (-2)       /* NxProcessKill 로 종료됨 */
#define NX_EXIT_SIGNAL(sig) (-256 - (int)(sig))   /* 신호의 기본 동작으로 종료됨 (SIGINT = Ctrl-C 등) */
#define NX_EXIT_IS_SIGNAL(code) ((code) <= -257 && (code) >= -256 - 31)

#define NX_NO_TOKEN        0UL        /* NxProcessCreate: 토큰 없음 (부모 신원 상속). 0 은 stdin 이라 토큰일 수 없다 */

/* uid/gid 예약 범위 (2^31 이상). 계정으로 쓸 수 없다 (커널 auth 가 그런 줄을 거부한다) */
#define NX_UID_RESERVED_MIN 0x80000000U
/* roi:/default/ 틀에서 "새 사용자 소유" 를 뜻하는 소유자. 복사할 때 그 사용자의 uid/gid 로 바뀐다 */
#define NX_UID_TEMPLATE     0xFFFFFFFEU
#define NX_AUTH_FAIL_DELAY_MS 1000UL  /* 인증 실패 시 지연 (무차별 대입을 느리게) */
#define NX_POLL_MAX        32UL       /* 2.7: 16 -> 32 */
#define NX_CHAN_MSG_MAX    256UL      /* 채널 메시지 최대 바이트 */
#define NX_CHAN_QUEUE_MAX  16UL       /* 채널 큐 최대 메시지 수 */
#define NX_CHAN_PUBLIC     0x1UL      /* NxChannelCreate: 누구나 보낼 수 있음 */
#define NX_SHM_MAX         (16UL * 1024UL * 1024UL)    /* 공유 메모리 객체 하나 */
#define NX_SHM_TOTAL_MAX   (128UL * 1024UL * 1024UL)   /* 모든 공유 메모리 합계 */
#define NX_HANDLE_INVALID_VALUE 0xFFFFFFFFFFFFFFFFUL   /* "핸들 없음" (nx_msginfo_ex.handle) */
#define NX_BLOCK_SECTOR    512UL      /* 블록 장치 읽기/쓰기 단위 (오프셋/길이 모두 배수여야 함) */
#define NX_PIPE_CAPACITY   4096U      /* 파이프 버퍼 */
#define NX_PIPE_ATOMIC     512U       /* 이 이하의 파이프 쓰기는 섞이지 않는다 (POSIX PIPE_BUF) */
#define NX_UMASK_DEFAULT   022U

/* NxPoll 이벤트 */
#define NX_POLL_IN         0x1U
#define NX_POLL_OUT        0x2U
#define NX_POLL_EXIT       0x4U
#define NX_POLL_HUP        0x8U

/* NxCreateNode 종류 */
#define NX_NODE_FILE       1UL
#define NX_NODE_DIR        2UL

/* NxDeviceCommand */
#define NX_DEVCMD_INFO     1UL        /* arg = struct nx_devinfo * */
#define NX_DEVCMD_IOPERM   2UL        /* arg = 1 허용 / 0 회수 : 이 장치의 I/O 포트를 ring3 에서 직접 in/out */
#define NX_DEVCMD_TTY_ECHO 3UL        /* 콘솔(stdin) 핸들에: arg = 1 에코 켬 / 0 끔 (비밀번호 입력) */
/*
 * 콘솔 핸들(0/1/2 어느 것이든)에: arg = NX_TTY_MODE_*.
 *   COOKED 기본. 커널 줄 편집기가 한 줄씩 넘긴다 (tty.h).
 *   RAW    줄 편집/에코 없음. 입력 바이트를 바로 넘기고 읽기는 있는 만큼(최대 len) 돌려준다.
 *          키보드의 방향키/Home/End/Delete/PgUp/PgDn 은 VT 시퀀스(ESC [ A ...)로, Enter 는 '\r' 로 온다.
 * 모드를 바꾸면 아직 읽지 않은 입력은 버린다 (TCSAFLUSH). RAW 를 켠 프로세스가 끝나면 커널이 COOKED 로 되돌린다.
 */
#define NX_DEVCMD_TTY_MODE    4UL
#define NX_DEVCMD_TTY_WINSIZE 5UL     /* 콘솔 핸들에: arg = struct nx_winsize * (글자 칸 행/열) */
/*
 * 콘솔 핸들에: 포그라운드 프로세스 그룹. arg = pgid (0 = 없음) 로 정하거나 NX_PGID_QUERY 로 조회 -> 지금 값.
 * cooked 모드에서 Ctrl-C 를 누르면: 쓰던 줄과 읽지 않은 입력을 버리고 "^C" 를 보인 뒤
 *   - 포그라운드 그룹이 있으면 그 그룹의 프로세스에 SIGINT 를 보낸다 (Ctrl-\ 는 SIGQUIT). 처리기가 없으면
 *     기본 동작으로 끝난다 (종료 코드 NX_EXIT_SIGNAL(SIGINT)).
 *   - 없으면(셸이 프롬프트에서 기다리는 중) 빈 줄("\n")을 넘긴다 (셸이 새 프롬프트를 띄우도록).
 * raw 모드에서는 Ctrl-C 도 0x03 바이트로 그대로 간다 (편집기 등이 직접 처리).
 * 그룹을 정할 수 있는 조건: root 이거나, 그 그룹의 살아 있는 프로세스가 모두 호출자와 같은 uid
 * (다른 사용자의 프로세스(init, login 등)를 Ctrl-C 로 끝내게 만들 수 없다). 아니면 Npermission.
 */
/* pty 주 끝에는 조회(NX_PGID_QUERY)만 된다 (종속 끝의 포그라운드 그룹) */
#define NX_DEVCMD_TTY_FOREGROUND 6UL
/* dev:/console 에: arg = struct nx_fontinfo * (buf 가 0 이면 크기만 채운다) */
#define NX_DEVCMD_CONSOLE_FONT   7UL
/* pty 주 끝 또는 종속 끝에: arg = const struct nx_winsize * (창 크기를 정한다. 콘솔은 Nunsupported) */
#define NX_DEVCMD_TTY_SET_WINSIZE 8UL
#define NX_TTY_MODE_COOKED 0UL
#define NX_TTY_MODE_RAW    1UL

/* 장치 종류 (nx_devinfo.kind) */
#define NX_DEV_STREAM      1U
#define NX_DEV_BLOCK       2U
#define NX_DEV_FRAMEBUFFER 3U
#define NX_DEV_CONSOLE     4U     /* dev:/console: 프레임버퍼 + 키보드 */

/* nx_devinfo.format: 픽셀 하나(32비트 little-endian 정수)의 배치 */
#define NX_FB_FORMAT_XRGB  1U     /* 0x00RRGGBB (메모리 순서 B G R X) */
#define NX_FB_FORMAT_XBGR  2U     /* 0x00BBGGRR (메모리 순서 R G B X) */

/*
 * 이름공간 (NxOpen / NxCreateNode / NxRemoveNode / NxRenameNode / NxReadDir / NxProcessCreate 의 경로)
 *   fs:/    전체 파일 트리. uid 0 만 직접 쓸 수 있다
 *   app:/   = fs:/usr/          실행 파일
 *   face:/  = fs:/home/<사용자>/  호출한 프로세스의 사용자 홈 (프로세스마다 다르다)
 *   roi:/   = fs:/roi/          운영체제 구동용 레지스트리 (파일 하나에 값 하나). uid 0 은 전부,
 *                               그 외 사용자는 roi:/<자기 uid>/ 아래만 (나머지는 Npermission).
 *                               roi:/users 는 커널 전용(아무도 열 수 없음). 구조:
 *                                 roi:/hostname       호스트 이름 (root 가 고친다, info:/hostname 으로 공개)
 *                                 roi:/timezone       시간대 이름 (기본 Europe/London, info:/timezone 으로 공개).
 *                                                     이름뿐이다: 시각은 늘 UTC (오프셋/서머타임 없음)
 *                                 roi:/default/       새 사용자용 틀 (소유자 NX_UID_TEMPLATE = 새 사용자)
 *                                 roi:/<uid>/         0500 본인: 목록/들어가기만
 *                                 roi:/<uid>/app/     0700 본인: 앱 설정 (자유롭게 읽고 씀)
 *                                 roi:/<uid>/shell    0444 root: 관리자가 정한 로그인 셸 (app:/...)
 *   info:/                      커널이 만드는 읽기 전용 가상 파일 (누구나). 파일 하나에 값 하나 + '\n'.
 *                               열 때 내용을 만들어 둔다(스냅샷): 새 값은 다시 열어서. 쓰기/만들기/지우기는
 *                               Npermission. 항목: hostname timezone utc_offset(늘 +00:00) version abi uptime_ms memory/total_kib
 *                               memory/free_kib memory/ram_kib memory/reserved_kib. 디렉터리(info:/, info:/memory)는 NxReadDir 로 나열.
 *   tmp:/   재부팅하면 사라지는 RAM 파일시스템 (누구나)
 *   disk:/  재부팅해도 남는 SATA 디스크의 nyfs (있을 때만. 권한은 디스크의 모드/소유자대로)
 *   dev:/   장치 (위 참고. root 만, 표준 스트림 장치는 누구나), svc:/ 채널 (위 참고)
 *   "." 는 무시하고, 별칭의 뿌리 위로 올라가는 ".." 는 Npermission. 접두어가 없거나 모르는 접두어는 NnotFound.
 */

/* NxVirtualAlloc 남용을 막는 상한(기초적인 DoS 방지). 물리 프레임 할당자는 유한하므로
 * 프로세스 하나가 한 번의 호출로, 또는 누적으로 전체 RAM 을 다 가져가지 못하게 한다. */
#define NX_HEAP_GROW_MAX_PER_CALL (256UL * 1024UL * 1024UL)  /* 한 번의 NxVirtualAlloc 상한: 256MiB */
#define NX_HEAP_MAX_TOTAL         (512UL * 1024UL * 1024UL)  /* 프로세스당 힙 총량 상한: 512MiB */

/* ---------------- 구조체 ---------------- */

/* 파일 종류 */
#define NX_TYPE_FILE     1U
#define NX_TYPE_DIR      2U
#define NX_TYPE_CONSOLE  3U
#define NX_TYPE_PROCESS  4U
#define NX_TYPE_TOKEN    5U
#define NX_TYPE_CHANNEL  6U
#define NX_TYPE_DEVICE   7U
#define NX_TYPE_PIPE     8U
#define NX_TYPE_PTY      9U     /* 의사 터미널 주 끝 */
#define NX_TYPE_SHM      10U    /* 공유 메모리 */

/* NxStat: 32바이트 */
struct nx_stat {
    uint64_t size;
    uint32_t type;        /* NX_TYPE_* */
    uint32_t rights;      /* 이 핸들이 가진 NX_RIGHT_* (NxStatPath 는 0) */
    uint32_t mode;        /* 파일/디렉터리의 권한 비트 (0777 이하). 그 밖의 종류는 0 */
    uint32_t uid;         /* 소유자 (파일/디렉터리만) */
    uint32_t gid;
    uint32_t reserved0;   /* 0 */
};

/* NxSysInfo: 48바이트 */
struct nx_sysinfo {
    uint32_t abi_version_major;
    uint32_t abi_version_minor;
    uint32_t page_size;
    uint32_t timer_hz;
    uint64_t ram_bytes;       /* 사용 가능한 RAM 의 끝 주소(대략적인 총량) */
    uint64_t uptime_ns;
    uint32_t process_count;   /* 살아 있는 프로세스 수 */
    uint32_t reserved0;
    uint64_t reserved1;
};

/* NxProcessInfo: 32바이트 */
/* NxSignalAction (24 바이트) */
struct nx_sigaction {
    uint64_t handler;         /* NX_SIG_DFL / NX_SIG_IGN / 처리기 주소 */
    uint64_t restorer;        /* 처리기가 돌아갈 주소 (NxSignalReturn 을 부른다) */
    uint32_t mask;            /* 처리하는 동안 더 막을 신호 */
    uint32_t flags;           /* NX_SA_* */
};

/* 처리기의 두 번째 인자 (128 바이트, 앞부분 배치는 Linux 의 siginfo_t 와 같다) */
struct nx_siginfo {
    int32_t  signo;
    int32_t  err;             /* 늘 0 */
    int32_t  code;            /* NX_SI_* */
    int32_t  pad;
    union {
        struct { uint32_t pid; uint32_t uid; } proc;   /* NX_SI_USER, NX_SI_CHILD */
        uint64_t addr;                                 /* NX_SI_FAULT */
    } u;
    uint8_t  reserved[104];
};

/* 처리기의 세 번째 인자: 신호가 오기 직전의 상태. NxSignalReturn 이 되돌린다 (처리기가 고치면 고친 대로) */
struct nx_sigcontext {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t rip, rflags, rsp;
    uint32_t mask;            /* 처리기가 끝나면 돌아갈 막은 집합 */
    uint32_t pad;
    uint8_t  fpu[512];        /* FXSAVE64 형식 */
};

/* 사용자 스택에 쌓이는 모양. 처리기가 시작할 때 rsp = &ret (rsp % 16 == 8, 함수 호출 직후와 같다) */
struct nx_sigframe {
    uint64_t ret;             /* = restorer */
    struct nx_sigcontext ctx;
    struct nx_siginfo info;
};

struct nx_procinfo {
    uint32_t pid;
    uint32_t state;           /* 0=READY 1=RUNNING 2=BLOCKED 3=TERMINATED */
    uint32_t handle_count;    /* 열려 있는 핸들 수 */
    uint32_t parent_pid;      /* 만든 프로세스의 pid (커널이 만들었으면 0) */
    uint64_t reserved[2];
};

/* NxGetIdentity: 64바이트 */
struct nx_identity {
    uint32_t uid;
    uint32_t gid;
    char     name[56];        /* NUL 종료 */
};

/* NxPoll 항목: 16바이트 */
struct nx_pollfd {
    uint64_t handle;
    uint32_t events;          /* 기다릴 NX_POLL_* */
    uint32_t revents;         /* 커널이 채움 */
};

/* NxChannelReceive 메시지 정보: 16바이트 */
struct nx_msginfo {
    uint32_t sender_pid;
    uint32_t sender_uid;
    uint32_t sender_gid;
    uint32_t length;          /* 메시지 길이 (버퍼가 작아 실패했을 때도 채워진다) */
};

/* NxChannelReceiveEx 메시지 정보: 24바이트 */
struct nx_msginfo_ex {
    uint32_t sender_pid;
    uint32_t sender_uid;
    uint32_t sender_gid;
    uint32_t length;
    uint64_t handle;          /* 메시지에 붙어 온 핸들, 없으면 NX_HANDLE_INVALID_VALUE */
};

/* NxReadDir: 256바이트 */
struct nx_dirent {
    uint32_t type;            /* NX_TYPE_FILE / NX_TYPE_DIR */
    uint32_t reserved;
    char     name[248];       /* NUL 종료 */
};

/* NX_DEVCMD_TTY_WINSIZE: 8바이트. 프레임버퍼 콘솔이 없으면 24 x 80 */
struct nx_winsize {
    uint16_t rows;
    uint16_t cols;
    uint16_t xpixel;          /* 화면 픽셀 크기 (모르면 0) */
    uint16_t ypixel;
};

/* NX_DEVCMD_INFO: 40바이트 */
struct nx_devinfo {
    uint32_t kind;            /* NX_DEV_* */
    uint32_t format;          /* 프레임버퍼: NX_FB_FORMAT_* (그 밖의 장치는 0) */
    uint64_t size;            /* 블록 장치/프레임버퍼 바이트 수 */
    uint32_t width;           /* 프레임버퍼 */
    uint32_t height;
    uint32_t pitch;           /* 한 줄의 바이트 수 */
    uint32_t bpp;
    uint16_t io_base;         /* 스트림 장치의 I/O 포트 범위 (NX_DEVCMD_IOPERM 대상) */
    uint16_t io_count;
    uint32_t reserved1;
};

/*
 * dev:/console 읽기: 키 이벤트 8바이트.
 * code 는 PS/2 스캔코드 세트 1 의 누름 코드이고, E0 으로 시작하는 확장 키는 0x100 을 더한다 (NX_KEY_*).
 * 키를 누르고 있으면 자동 반복으로 pressed = 1 이 여러 번 온다.
 */
struct nx_keyevent {
    uint16_t code;            /* NX_KEY_* */
    uint16_t mods;            /* NX_KMOD_*: 이 이벤트를 반영한 뒤의 수정자 상태 */
    uint8_t  pressed;         /* 1 누름, 0 뗌 */
    uint8_t  ch;              /* Shift/Caps Lock 을 반영한 ASCII (글자가 아닌 키는 0). Ctrl/Alt 는 반영하지 않는다 */
    uint16_t reserved;
};

#define NX_KMOD_SHIFT  0x01U
#define NX_KMOD_CTRL   0x02U
#define NX_KMOD_ALT    0x04U      /* 왼쪽 Alt */
#define NX_KMOD_SUPER  0x08U      /* Windows / Command 키 */
#define NX_KMOD_ALTGR  0x10U      /* 오른쪽 Alt */
#define NX_KMOD_CAPS   0x20U      /* Caps Lock 켜짐 */

#define NX_KEY_ESC        0x001U
#define NX_KEY_BACKSPACE  0x00EU
#define NX_KEY_TAB        0x00FU
#define NX_KEY_ENTER      0x01CU
#define NX_KEY_LCTRL      0x01DU
#define NX_KEY_LSHIFT     0x02AU
#define NX_KEY_RSHIFT     0x036U
#define NX_KEY_LALT       0x038U
#define NX_KEY_SPACE      0x039U
#define NX_KEY_CAPSLOCK   0x03AU
#define NX_KEY_F1         0x03BU     /* F1..F10 = 0x3B..0x44 */
#define NX_KEY_F10        0x044U
#define NX_KEY_F11        0x057U
#define NX_KEY_F12        0x058U
#define NX_KEY_KP_ENTER   0x11CU
#define NX_KEY_RCTRL      0x11DU
#define NX_KEY_RALT       0x138U
#define NX_KEY_HOME       0x147U
#define NX_KEY_UP         0x148U
#define NX_KEY_PGUP       0x149U
#define NX_KEY_LEFT       0x14BU
#define NX_KEY_RIGHT      0x14DU
#define NX_KEY_END        0x14FU
#define NX_KEY_DOWN       0x150U
#define NX_KEY_PGDN       0x151U
#define NX_KEY_INSERT     0x152U
#define NX_KEY_DELETE     0x153U
#define NX_KEY_LSUPER     0x15BU
#define NX_KEY_RSUPER     0x15CU
#define NX_KEY_MENU       0x15DU
#define NX_KEY_PRTSC      0x137U     /* PrintScreen (Alt 와 함께면 SysRq: 커널이 쓴다) */

/* NxProcessList 항목: 112바이트 */
struct nx_procent {
    uint32_t pid;
    uint32_t parent_pid;
    uint32_t pgid;
    uint32_t uid;
    uint32_t state;           /* 0=READY 1=RUNNING 2=BLOCKED (끝난 프로세스는 나오지 않는다) */
    uint32_t flags;           /* NX_PROCENT_* */
    uint64_t cpu_ticks;       /* CPU 를 쓴 타이머 틱 수 (NxSysInfo 의 틱 단위) */
    uint64_t start_tick;      /* 만든 시각 (부팅 후 틱) */
    uint64_t mem_kib;         /* 사용자 주소 공간에 매핑된 메모리 (장치 MMIO 제외) */
    char     name[32];        /* 실행 파일 이름 (경로의 마지막 이름, ".run" 없이). 커널 스레드는 "idle"/"kernel" */
    char     user[32];        /* 사용자 이름 */
};
#define NX_PROCENT_USER    0x1U   /* 사용자 프로세스 (아니면 커널 스레드) */
#define NX_PROCENT_SESSION 0x2U   /* 콘솔 세션 (로그인 셸) */
#define NX_PROCENT_PTY     0x4U   /* 제어 터미널이 의사 터미널 */
#define NX_PROCENT_CRITICAL 0x8U  /* 보호 프로세스 (init, login: NX_SPAWN_CRITICAL) */

/* NX_DEVCMD_CONSOLE_FONT: 32바이트. 글자 하나 = cell_w * cell_h 바이트의 알파(0 배경 ~ 255 글자색), 줄 순서 */
struct nx_fontinfo {
    uint64_t buf;             /* 받을 사용자 버퍼 (0 이면 크기만) */
    uint64_t cap;             /* buf 의 크기 (count * cell_w * cell_h 이상이어야 한다) */
    uint32_t cell_w;          /* 커널이 채운다 */
    uint32_t cell_h;
    uint32_t first;           /* 첫 글자 코드 (32) */
    uint32_t count;           /* 글자 수 (95: ' ' ~ '~') */
};

#endif /* NYX_ABI_H */
