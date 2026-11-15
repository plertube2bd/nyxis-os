/*
 * nxbw.h - Nyxis 흑백 동영상 코덱 "nxbw" 와 사각파 음 트랙 "nxsq" 의 정의
 *
 * 플레이어(userland/badapple, Nyxis 안)와 인코더(tools/badapple/nxbwenc.c, 호스트)가 함께 쓴다.
 * 컨테이너는 보통의 MP4 (ISO BMFF: ftyp, mdat, moov) 이고, 안의 샘플만 아래의 자체 형식이다.
 * 자세한 설명: docs/BADAPPLE.md
 *
 * 비디오 트랙 (hdlr 'vide', 샘플 엔트리 'nxbw', 한 샘플 = 한 프레임)
 *   화면: NXBW_W x NXBW_H 픽셀, 픽셀당 4비트 = 16단계 흑백 (0 = 검정 .. 15 = 흰색)
 *   풀린 프레임: 한 줄씩 위에서 아래로, 한 바이트에 두 픽셀 (왼쪽 픽셀 = 위 4비트) = NXBW_FRAME_BYTES 바이트
 *   샘플 = 1바이트 종류 + 내용
 *     NXBW_KEY   내용 = zlib 스트림 (풀면 프레임 그대로)
 *     NXBW_DELTA 내용 = zlib 스트림 (풀면 "이전 프레임 XOR 이 프레임": 바뀌지 않은 곳은 0 이라 잘 줄어든다)
 *     NXBW_SAME  내용 없음 (이전 프레임과 같다)
 *   압축은 deflate (zlib 형식, adler32 확인): 인코더와 플레이어 모두 uzlib (third_party/uzlib)
 *   첫 프레임과 stss(동기 샘플)에 적힌 프레임은 NXBW_KEY 다
 *   샘플 엔트리 'nxbw' 의 자식 상자 'nxbC' = struct 처럼 큰 끝(big endian):
 *     u8 version(1), u8 bits_per_pixel(4), u8 levels(16), u8 compression('z' = zlib),
 *     u16 width, u16 height, u32 key_interval
 *
 * 음 트랙 (hdlr 'soun', 샘플 엔트리 'nxsq', 한 샘플 = 비디오 한 프레임 동안)
 *   샘플 = u16 큰 끝 주파수 (Hz, 0 = 그 목소리 쉼) 를 목소리 수만큼. 목소리 = 선율, 화성음, 베이스 (이 순서)
 *   한 프레임의 음들이 화음이다: Nyxis 는 PC 스피커 하나로 10ms 마다 음을 바꿔 (아르페지오) 낸다
 *   자식 상자 'nxsC': u8 version(2), u8 waveform('s' = square), u8 voices(3), u8 reserved
 *     (version 1 은 목소리 하나: u8 1, u8 's', u16 0. 플레이어는 둘 다 읽는다)
 */
#ifndef NYXIS_NXBW_FORMAT_H
#define NYXIS_NXBW_FORMAT_H

#define NXBW_W              80
#define NXBW_H              60
#define NXBW_BPP            4
#define NXBW_LEVELS         16
#define NXBW_FRAME_BYTES    (NXBW_W * NXBW_H * NXBW_BPP / 8)

#define NXBW_KEY            0x00
#define NXBW_DELTA          0x01
#define NXBW_SAME           0x02

#define NXBW_FOURCC(a, b, c, d) \
    (((unsigned long)(a) << 24) | ((unsigned long)(b) << 16) | ((unsigned long)(c) << 8) | (unsigned long)(d))

#define NXBW_VIDEO_FOURCC   NXBW_FOURCC('n', 'x', 'b', 'w')
#define NXBW_VCONF_FOURCC   NXBW_FOURCC('n', 'x', 'b', 'C')
#define NXBW_AUDIO_FOURCC   NXBW_FOURCC('n', 'x', 's', 'q')
#define NXBW_ACONF_FOURCC   NXBW_FOURCC('n', 'x', 's', 'C')
#define NXBW_MAX_VOICES     4

/* 픽셀 (x, y) 의 단계 (0..15) */
#define NXBW_PIXEL(frame, x, y) \
    ((((x) & 1) ? (frame)[((y) * NXBW_W + (x)) >> 1] : ((frame)[((y) * NXBW_W + (x)) >> 1] >> 4)) & 0x0F)

#endif /* NYXIS_NXBW_FORMAT_H */
