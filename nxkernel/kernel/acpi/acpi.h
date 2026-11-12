/*
 * acpi.h - ACPI 전원 끄기 (S5)
 *
 * 부팅 때(acpi_init) 부트로더가 넘긴 RSDP(boot info 의 Rsdp, HHDM 주소)에서 RSDT/XSDT -> FADT(FACP) 를 찾고,
 * DSDT 안의 \_S5 패키지에서 SLP_TYPa/b 를 읽어 PM1a/PM1b 제어 레지스터에 SLP_TYP|SLP_EN 을 쓴다.
 * ACPI 모드가 아니면(SCI_EN=0) SMI_CMD 에 ACPI_ENABLE 을 써서 먼저 켠다.
 * AML 해석기는 없다: \_S5 는 거의 모든 펌웨어가 단순한 Name(_S5, Package(){...}) 형태로 두므로
 * 그 바이트 패턴만 읽는다. 실패하면 QEMU/Bochs 의 고정 포트로 한 번 더 시도한다.
 *
 * 호출 권한(uid 0)은 시스템 콜 계층이 검사한다. 정책(누가 끌 수 있나)은 init 이 정한다.
 */
#ifndef KERNEL_ACPI_ACPI_H
#define KERNEL_ACPI_ACPI_H

#include "nyxis.h"

/*
 * 테이블을 읽어 S5 에 필요한 값(PM1a/b 포트, SLP_TYPa/b, SMI_CMD/ACPI_ENABLE)만 커널에 복사해 둔다.
 * pfa_init() 전에(HHDM 이 켜진 뒤) 한 번 호출한다: pfa 가 UEFI BootServices/Loader 영역을 회수하므로,
 * 그런 영역에 테이블을 두는 펌웨어라도 나중에 덮어쓰인 메모리를 읽지 않도록. 콘솔 전에 불리므로 출력하지 않는다.
 */
Nstatus acpi_init(const void *rsdp_virt);   /* NnotFound: RSDP/FADT 없음, Nunsupported: \_S5/PM1 없음 */

/* 성공하면 돌아오지 않는다. 돌아오면 전원을 끄지 못한 것 (Nunsupported 등) */
Nstatus acpi_power_off(void);

#endif /* KERNEL_ACPI_ACPI_H */
