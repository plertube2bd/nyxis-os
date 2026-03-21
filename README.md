# Nyxis_OS
Nyxis OS is 64bit x86 OS, powerful API

# 닉스 운영체제가 무엇인가요?
닉스 운영체제는 강력한 중앙 API를 이용하여 프로그램의 동작을 일관되고 정의 가능하게 만드는 운영체제입니다.
닉스 운영체제는 리눅스를 참고한 자체 커널과 자체 API를 기반으로 하여, 객체 지향적 운영체제를 만들기 위해 노력합니다.

# 닉스 운영체제를 사용해야할 이유가 무엇인가요?
닉스 운영체제를 사용해야할 이유는 크지 않고, 닉스 운영체제는 실사용으로 개발중이지 않습니다.
닉스 운영체제는 객체지향적 커널의 모습을 보여줍니다. 객체 지향 프로그래밍이나 초보 커널 개발자에게 좋은 학습 데이터가 될 수 있습니다.

# 닉스 운영체제의 요구 사항은 무엇인가요?
당신의 컴퓨터에 UEFI가 있으며, 부팅이 정상적으로 된다면 거의 모든 x86 하드웨어에서 동작 가능합니다.
저장공간은 매우 중요합니다. 닉스 운영체제는 다음과 같은 저장공간 형태를 요구합니다:

## 가능한 것
SATA Hard Disk Drive(HDD)
SATA Solid State Drive(SSD)

## 불가능한 것
CD-R
CD-RW
DVD-R
DVD-RW
Blu-Ray
기타 광학 저장장치

PATA Hard Disk Drive(PATA HDD)
NVMe Hard Disk Drive(NVMe HDD)
SAS Hard Disk Drive(SAS HDD)
기타 하드 디스크 드라이브

NAND Flash (USB)
NOR Flash
NVMe Solid State Drive(NVMe SSD)
기타 플레시 메모리

## 파일 시스템
DOS File System(FAT)과, FAT32밖에 지원하지 않습니다. NyxFS가 만들어진다면 고칠 예정입니다.

# Nyxis OS를 시작하는 법

```Bash

$ git clone https://github.com/plertube2bd/nyxis-os.git
$ cd nyxis-os
$ make
# 아직 완성되지 않은 운영체제입니다.

```

---

# Nyxis_OS

Nyxis OS is a 64-bit x86 operating system with a powerful API.

# What is Nyxis OS?

Nyxis OS is an operating system designed to make program behavior consistent and well-defined through a centralized and powerful API.
It is based on a custom kernel inspired by Linux and its own API, aiming to create an object-oriented operating system.

# Why should you use Nyxis OS?

There are not many practical reasons to use Nyxis OS, as it is not being developed for everyday use.
Instead, Nyxis OS demonstrates the concept of an object-oriented kernel. It can serve as a good learning resource for those interested in object-oriented programming or beginner kernel developers.

# What are the requirements for Nyxis OS?

If your computer supports UEFI and can boot normally, it should work on most x86 hardware.

Storage is very important. Nyxis OS requires specific types of storage:

## Supported

* SATA Hard Disk Drive (HDD)
* SATA Solid State Drive (SSD)

## Not Supported

* CD-R

* CD-RW

* DVD-R

* DVD-RW

* Blu-ray

* Other optical storage devices

* PATA Hard Disk Drive (PATA HDD)

* NVMe Hard Disk Drive (NVMe HDD)

* SAS Hard Disk Drive (SAS HDD)

* Other types of hard drives

* NAND Flash (USB)

* NOR Flash

* NVMe Solid State Drive (NVMe SSD)

* Other flash memory

## File System

Only DOS File System (FAT) and FAT32 are supported. This may change if NyxFS is developed in the future.
