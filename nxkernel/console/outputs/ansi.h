/*
 * ansi.h - 터미널 출력 해석기 (ANSI/ECMA-48 이스케이프 시퀀스의 작은 부분집합)
 *
 * 사용자 출력(tty_write)의 바이트를 하나씩 받아 글자는 conout 으로, 제어 시퀀스는 해석해서
 * 프레임버퍼에 적용한다. 커널 로그(printk)는 이 해석기를 거치지 않는다.
 *
 * 지원하는 것 (그 밖의 시퀀스는 조용히 버린다):
 *   제어 문자   \n \r \t, \b (커서 왼쪽 한 칸, 지우지 않음), BEL(무시). 나머지 제어 바이트는 '?'
 *   ESC 7 / 8   커서 저장 / 복원          ESC c   초기화 (색 기본, 화면 지움, 커서 맨 위, 커서 보임)
 *   CSI n A/B/C/D  위/아래/오른쪽/왼쪽 n 칸     CSI n E/F  n 줄 아래/위의 첫 칸
 *   CSI n G     n 번째 열                     CSI n d    n 번째 행
 *   CSI r;c H   (= f) r 행 c 열 (1 부터)
 *   CSI n J     화면 지우기 (0 커서~끝, 1 처음~커서, 2/3 전체)
 *   CSI n K     줄 지우기 (0 커서~끝, 1 처음~커서, 2 전체)
 *   CSI s / u   커서 저장 / 복원
 *   CSI ... m   SGR: 0 초기화, 1/22 굵게(밝은 색), 7/27 반전, 30-37/90-97 글자색, 40-47/100-107 배경색,
 *               39/49 기본색, 38;5;n / 48;5;n (256색), 38;2;r;g;b / 48;2;r;g;b (트루컬러).
 *               그 밖의 속성(기울임, 밑줄 ...)은 무시
 *   CSI ?25 h/l 커서 보이기/숨기기 (프레임버퍼의 반전 블록 커서 + 시리얼로 전달)
 *   OSC/DCS/SOS/PM/APC (ESC ] P X ^ _) 문자열 시퀀스: 종료 문자(BEL 또는 ESC \)까지 통째로 버린다 (창 제목 등)
 *
 * 보안: 시리얼 터미널로는 받은 바이트를 그대로 넘기지 않는다. 위 목록에 있는 시퀀스만, 검증한 숫자로
 * 다시 만들어서 보낸다 (OSC/DCS 같은 위험한 시퀀스나 임의 바이트를 사용자 프로그램이 주입할 수 없다).
 * 시퀀스 하나는 최대 ANSI_MAX_SEQ 바이트, 매개변수 최대 ANSI_MAX_PARAMS 개, 각 값은 ANSI_PARAM_MAX 로 자른다.
 *
 * 재진입 불가: 호출자(tty_write)가 인터럽트를 막은 상태에서 부른다.
 */
#ifndef CONSOLE_OUTPUTS_ANSI_H
#define CONSOLE_OUTPUTS_ANSI_H

#include "nyxis.h"

#define ANSI_MAX_SEQ    64U
#define ANSI_MAX_PARAMS 16U
#define ANSI_PARAM_MAX  9999U
#define ANSI_MAX_STRING 512U   /* OSC/DCS 등 문자열 시퀀스 길이 상한 (넘으면 버리기를 그만둔다) */

/* 바이트 하나 해석 */
void ansi_feed(u8 c);

/* (사용자 출력 쪽) 해석 상태와 색 속성을 처음으로 (진행 중이던 시퀀스는 버린다) */
void ansi_reset(void);

/*
 * 커널 로그(printk) 전용 인스턴스. printk 가 begin -> feed... -> end 로 감싸서 부른다.
 * 서식 문자열(커널 코드 안의 리터럴)만 여기로 들어온다: %s 로 들어오는 외부 문자열은 printk 가 직접 거른다.
 * begin 은 커널 쪽 색을 기본값으로 시작하고, end 는 커널 로그가 색을 바꿨다면 사용자 출력 쪽 색을
 * 프레임버퍼와 시리얼에 되살린다.
 */
void ansi_kernel_begin(void);
void ansi_kernel_feed(u8 c);
void ansi_kernel_end(void);

#endif /* CONSOLE_OUTPUTS_ANSI_H */
