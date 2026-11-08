/*
 * taskbar.c - 작업 표시줄 (화면 맨 아래 한 줄)
 *
 * 아직 구현하지 않았다. 계획:
 *
 *   [ntui] [창1] [창2*] ...                     test@nyxis  Europe/London  12:34
 *
 * TODO: 그리기
 *   - void taskbarDraw(void): 맨 아래 줄(screenRows - 1)을 ROLE_TASKBAR 로 채우고 아래 항목을 그린다.
 *     창 영역은 screenRows - 1 줄까지로 줄인다 (wm 이 생기면 wm 에 알려 준다).
 *   - 왼쪽: 시작 메뉴 버튼 "[ntui]". 누르면(단축키, 예: Ctrl-Space 또는 ESC 두 번) 메뉴를 연다.
 *   - 가운데: 열린 창 목록. 활성 창은 ROLE_SELECTION, 나머지는 ROLE_TASKBAR. 넘치면 "..." 로 줄인다.
 *   - 오른쪽: username@hostname (info:/hostname), timezoneName, 시계 HH:MM.
 *     좁은 화면에서는 시계 > 사용자 > 시간대 순으로 남긴다.
 *
 * TODO: 시계
 *   - time() + utcOffset(분)으로 지역 시각을 계산한다 (nlibc localtime 은 UTC 뿐: sys.c 참고).
 *   - 분이 바뀔 때만 다시 그린다. 이벤트 루프가 getch 시간 초과(100ms)마다 taskbarTick() 을 부른다.
 *   - hostname 은 시작할 때 한 번 읽는다 (sys.c 에 readValue 가 있다: 공개 함수로 옮길 것).
 *
 * TODO: 입력
 *   - int taskbarKey(int key): 작업 표시줄이 포커스를 가졌을 때 ←/→ 로 창 고르기, Enter 로 전환.
 *     처리했으면 1, 아니면 0 (호출자가 다른 곳에 넘긴다).
 *   - 창 전환 단축키 (예: Ctrl-N / Ctrl-P) 는 포커스와 상관없이 여기서 받는다.
 *
 * TODO: 창 목록과의 연결
 *   - 창 관리(wm.c)가 생기면 창 이름/활성 여부를 받아 오는 함수만 쓴다 (작업 표시줄이 창 구조체를 직접 만지지 않게).
 *   - 그 전까지는 고정된 목록으로 그려 본다.
 *
 * TODO: 테마
 *   - 색은 ROLE_TASKBAR / ROLE_SELECTION 만 쓴다. Mono 에서도 활성 창이 반전으로 구분되는지 확인.
 */
#include "ntui.h"
