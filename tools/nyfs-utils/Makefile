# Makefile - nyfs-utils (mkfs.nyfs, nyfs-fuse)
#
# 두 가지 컴파일 규칙을 쓴다:
#   1) 코어 로직(nyfs_core.c) + mkfs.nyfs(mkfs_nyfs.c)
#        -std=c89 -pedantic -Wall -Wextra -Werror -Wstrict-prototypes
#        -Wmissing-prototypes -Wshadow -Wdeclaration-after-statement
#      NyxisOS 저장소 전체 빌드 규칙(nxkernel/Makefile)과 동일한 엄격도.
#      이 두 파일은 커널 nyfs.c 와 같은 온디스크 포맷 로직을 다루므로
#      "커널에 그대로 가져다 붙여도 이상하지 않은 스타일"을 유지한다.
#   2) FUSE 글루(nyfs_fuse.c) 하나만 예외: libfuse3 헤더(fuse3/fuse.h) 자체가
#      C99 API(가변 길이 배열은 안 쓰지만 // 없는 C99 스타일 초기화, stdint
#      의존 등)라 -std=c89 로는 헤더 자체가 깨진다. nxkernel/Makefile 도
#      호스트 테스트 빌드(NYX_HOST_TEST)에서 이미 -std=gnu99 를 쓰는 선례가
#      있어 그 관례를 그대로 따른다: -std=gnu99 -Wall -Wextra -Werror.

CC        ?= gcc
CFLAGS_C89 = -std=c89 -pedantic -Wall -Wextra -Werror \
             -Wstrict-prototypes -Wmissing-prototypes -Wshadow \
             -Wdeclaration-after-statement -Iinclude
CFLAGS_FUSE = -std=gnu99 -Wall -Wextra -Werror -Iinclude $(shell pkg-config --cflags fuse3)
LDFLAGS_FUSE = $(shell pkg-config --libs fuse3)

BUILD = build

.PHONY: all clean install test

all: $(BUILD)/mkfs.nyfs $(BUILD)/nyfs-fuse

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/nyfs_core.o: src/nyfs_core.c include/nyfs_core.h include/nyfs_disk.h include/nyfs_host.h | $(BUILD)
	$(CC) $(CFLAGS_C89) -c $< -o $@

$(BUILD)/mkfs_nyfs.o: src/mkfs_nyfs.c include/nyfs_core.h | $(BUILD)
	$(CC) $(CFLAGS_C89) -c $< -o $@

$(BUILD)/mkfs.nyfs: $(BUILD)/mkfs_nyfs.o $(BUILD)/nyfs_core.o
	$(CC) $(CFLAGS_C89) $^ -o $@

# nyfs_fuse.c 만 C99(gnu99)로 별도 컴파일한다(위 설명 참고).
$(BUILD)/nyfs_fuse.o: src/nyfs_fuse.c include/nyfs_core.h | $(BUILD)
	$(CC) $(CFLAGS_FUSE) -c $< -o $@

$(BUILD)/nyfs-fuse: $(BUILD)/nyfs_fuse.o $(BUILD)/nyfs_core.o
	$(CC) $(CFLAGS_FUSE) $^ -o $@ $(LDFLAGS_FUSE)

# 코어 로직만 따로 검증하는 호스트 유닛 테스트(포맷→생성→쓰기→읽기→
# readdir→삭제까지 실제 FUSE 마운트 없이 라이브러리 API로 직접 돌려본다)
$(BUILD)/test_nyfs_core: tests/test_nyfs_core.c $(BUILD)/nyfs_core.o
	$(CC) $(CFLAGS_C89) $^ -o $@

test: $(BUILD)/test_nyfs_core
	./$(BUILD)/test_nyfs_core

install: all
	install -Dm755 $(BUILD)/mkfs.nyfs $(DESTDIR)/usr/sbin/mkfs.nyfs
	install -Dm755 $(BUILD)/nyfs-fuse $(DESTDIR)/usr/bin/nyfs-fuse

clean:
	rm -rf $(BUILD)
