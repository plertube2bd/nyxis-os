# badapple: 흑백 아스키 아트 + 사각파 동영상

`badapple` 은 Nyxis 안에서 동영상을 **80x60 글자 칸의 흑백 아스키 아트**로 그리고, 소리는 **PC 스피커의 사각파
비프음**으로 내는 플레이어다 (기본 단음, `-c` 로 화음). Bad Apple!! PV 를 위해 만들었지만 같은 형식이면 어떤 영상이든 된다.

```
badapple                 app:/badapple.run/badapple.mp4 재생
badapple -m              소리 없이
badapple -c              화음 (아르페지오). 기본은 단음
badapple -f              ntui 창을 전체 화면으로
badapple face:/other.mp4 다른 파일
```

재생 중 키: `q` / `Esc` / `Ctrl-C` 끝, `Space` 멈춤/계속, `m` 소리 끄기/켜기, `c` 단음/화음 바꾸기.

## 동영상 만들기 (호스트)

동영상 파일은 저장소에 넣지 않는다. 원본 영상(아무 형식, ffmpeg 가 읽으면 된다)에서 만든다:

```bash
# 원본 받기 (예: yt-dlp. 저작권은 각자 확인)
yt-dlp -f "bv*[height<=360]+ba/b[height<=360]" -o badapple.mkv "https://www.youtube.com/watch?v=FtutLA63Cp8"

cd nxkernel
make badapple-midi                                  # 악보 -> media/badapple.mid (아래 "음악")
make badapple-movie BADAPPLE_SRC=../badapple.mkv     # -> media/badapple.mp4 (약 3.7 MB)
make run                                            # 이미지에 app:/badapple.run/badapple.mp4 로 들어간다
```

- `media/badapple.mp4` 가 있으면 이미지를 만들 때 넣고, 없으면 플레이어만 들어간다 (실행하면 이 안내를 보인다).
  다른 위치는 `make BADAPPLE_MP4=<파일> run`.
- `make badapple-movie` 는 확인용 파일도 남긴다: `out/badapple/notes.txt` (프레임마다 뽑은 선율/화성음/베이스),
  `out/badapple/preview.wav` (Nyxis 에서 들릴 기본 소리 그대로: 단음),
  `out/badapple/preview-chord.wav` (`badapple -c` 의 소리: 10ms 아르페지오 화음).
- 인코더 `tools/badapple/nxbwenc.c` 는 호스트 C 프로그램이다 (`make badapple-enc` -> `out/host/nxbwenc`).

### 화면 크기

영상은 80x60 픽셀(4:3)이다. 콘솔 글꼴의 글자 칸은 8x16 (가로 1 : 세로 2) 이라 픽셀 하나를 **가로 글자 2개**로
그린다: 160열 x 60행 = 1280x960 화소 = 4:3. 그래서 화면 높이가 960 화소(60줄) 이상이면 1:1 로 보인다.

- 부트로더는 기본(`RES=auto`)으로 UEFI 가 주는 가장 큰 프레임버퍼 모드를 고른다. 원하는 크기는 `make RES=1600x1024 run`.
- 터미널이 160x60 보다 작으면 4:3 비율을 지키며 줄여 그린다 (위아래나 양옆은 검은 여백).
- ntui 에서는 창을 160x60 으로 맞춰 달라고 하고, 창 영역에 들어가지 않으면 전체 화면으로 바꾼다.

### 소리 (QEMU)

PC 스피커는 QEMU 의 `pcspk` 이고, 호스트 오디오 백엔드에 이어야 들린다. `make run` 은 `QEMU_AUDIO=auto` 로
이 QEMU 에 있는 백엔드(pipewire > pa > alsa > sdl)를 고른다. 하나도 없으면 소리 없이 돈다.

```bash
sudo pacman -S qemu-audio-pipewire      # Arch: 백엔드는 따로 설치한다 (또는 qemu-audio-pa, qemu-audio-alsa)
make run QEMU_AUDIO=wav                 # out/pcspk.wav 로 녹음 (검사용)
make run QEMU_AUDIO=none                # 끄기
```

## 파일 형식

컨테이너는 보통의 **MP4** (ISO BMFF: `ftyp`, `mdat`, `moov`) 다. `ffprobe` 같은 도구도 트랙, 크기, 길이, 프레임 수를 읽는다
(코덱은 모른다고 나온다). 안의 샘플만 Nyxis 자체 형식이다. 정의는 [userland/badapple/nxbw.h](../userland/badapple/nxbw.h).

### 비디오 트랙 `nxbw`

| 항목 | 값 |
|---|---|
| hdlr / 샘플 엔트리 | `vide` / `nxbw` (VisualSampleEntry, 자식 상자 `nxbC`) |
| 화면 | 80 x 60 픽셀, 30 fps (mdhd timescale 30, 샘플 하나 = 1) |
| 픽셀 | **4비트 = 16단계 흑백** (0 검정 .. 15 흰색). 한 바이트에 두 픽셀 (왼쪽 = 위 4비트) |
| 풀린 프레임 | 2400 바이트 (80 x 60 / 2) |
| 샘플 | 1바이트 종류 + 내용. `0` 키 프레임: zlib(프레임), `1` 델타: zlib(이전 프레임 XOR 이 프레임), `2` 같음: 내용 없음 |
| 키 프레임 | 첫 프레임과 300 프레임(10초)마다. `stss` 에 적는다 |
| 압축 | deflate, zlib 형식 (adler32 확인). 인코더와 플레이어 모두 **uzlib** ([third_party/uzlib](../third_party/uzlib)) |

`nxbC` 상자 (큰 끝): `u8 version=1, u8 bpp=4, u8 levels=16, u8 compression='z', u16 width, u16 height, u32 key_interval`.

델타 프레임은 바뀐 곳만 0 이 아니라서 deflate 로 아주 작아진다. Bad Apple!! (6572 프레임, 3분 39초)는 풀린 크기
15.8 MB 가 3.7 MB 가 된다.

### 음악: 악보 (기본)

`media/badapple.mid` 가 있으면 음은 **사람이 만든 악보**에서 온다: [xqe2011/bad-apple-midi](https://github.com/xqe2011/bad-apple-midi)
의 `input.mid` (단일 부저용으로 다듬은 Bad Apple!! 편곡, 원본은 [cnlohr/badderapple](https://github.com/cnlohr/badderapple)
의 MIDI. 그 저장소는 이 파일을 퍼블릭 도메인으로 공개한다). `make badapple-midi` 가 커밋을 고정해 받는다
(저장소에는 넣지 않는다. 곡 자체의 저작권은 원작자에게 있다).

- 863 음, 138 BPM, 1.3~220 초. 템포가 PV 와 같아 오프셋 0 으로 맞는다 (자동 추출 선율과 맞춰 본 결과 곡 앞/중간/끝
  모두 ±20ms 안). 다른 악보는 `BADAPPLE_MIDI=<파일>`, 어긋나면 `BADAPPLE_MIDI_OFFSET=<ms>`.
- 프레임마다 그 순간 울리는 음 하나 (겹치면 나중에 시작한 음). 음의 마지막 프레임은 쉼으로 둬서 같은 음이 이어져도
  끊어져 들린다.
- 인코더 옵션: `nxbwenc -M 악보.mid [-t ms]` (표준 MIDI 형식 0/1, 템포 바뀜 지원, 타악기 채널 제외).

악보가 없으면 아래처럼 원본 오디오에서 자동으로 뽑는다 (품질은 악보보다 많이 떨어진다).

### 음 트랙 `nxsq` (화음)

| 항목 | 값 |
|---|---|
| hdlr / 샘플 엔트리 | `soun` / `nxsq` (AudioSampleEntry, 자식 상자 `nxsC`: `u8 version=2, u8 waveform='s', u8 voices=3, u8 0`) |
| 샘플 | 비디오 프레임 하나 동안의 화음: 목소리마다 `u16` 큰 끝 주파수 (Hz, 0 = 그 목소리 쉼). 순서 = 선율, 화성음, 베이스 (6 바이트) |
| version 1 | 목소리 하나 (`u16` 하나). 플레이어는 둘 다 읽는다 |

화음은 인코더가 원곡에서 뽑는다. 프레임마다 그 시각의 오디오(11025 Hz, 2048 샘플, Hann 창)에서 반음마다(E2..A5)
배음 6개의 크기를 더한 현저도를 구한다.

| 목소리 | 음역 | 고르는 법 |
|---|---|---|
| 선율 | A3..A5 (노래) | 현저도가 크면서 앞 음에서 멀리 뛰지 않는 길 (비터비) |
| 화성음 | A3..A5 | 선율 다음으로 두드러진 음 (선율과 3반음 이상, 옥타브가 아닌 것. 3 프레임 넘게 이어질 때만) |
| 베이스 | E2..G#3 | 비터비 (선율과 같은 방법) |

조용하거나 음높이가 뚜렷하지 않은 곳은 쉼이다. 여러 악기가 섞인 음원에서 자동으로 뽑은 것이라 원곡 악보와
똑같지는 않다 (Bad Apple!!: 세 목소리 모두 음의 81~89% 가 원곡의 조(E♭ 단조) 안).

**PC 스피커로 화음 내기:** 스피커는 1비트 출력 하나라 음을 섞을 수 없다. 그래서 커널이 칩튠처럼 **아르페지오**로 낸다:
타이머 tick(10ms)마다 화음의 다음 음으로 바꾼다 (3음이면 30ms 에 한 바퀴). 어느 순간이든 사각파는 하나라 음이 겹치지
않고, 귀에는 화음으로 들린다. QEMU 녹음으로 확인한 모양: `beep -c 523,659,784` -> 523, 659, 784 Hz 가 10ms 씩 번갈아.

**그래서 기본은 단음이다:** 스피커는 켜짐/꺼짐뿐이라 **음량을 나눌 수 없다**. 화음의 음이 모두 같은 크기로 들려서,
원곡에서 작게 깔리던 화성음과 베이스가 노래만큼 커진다. `badapple` 은 프레임마다 가장 두드러진 음 하나(선율, 선율이
쉬면 베이스)만 낸다. 화음은 `badapple -c`, 재생 중 `c`, 또는 설정 `sound` 를 `chord` 로.

## 플레이어

- [userland/badapple/main.c](../userland/badapple/main.c): 파일을 읽고, MP4 표를 풀고([mp4.c](../userland/badapple/mp4.c)),
  단조 시계에 맞춰 프레임을 푼다. 늦으면 그리지 않고 풀기만 해서 따라잡는다 (델타 프레임이라 건너뛸 수 없다).
- 그리기: nx64api TUI. 흰 글자 / 검은 바탕, 밝기 16단계를 글자 16개로 (설정 `ramp`, 기본 ` .,:;-=+*ox%#&@M`).
- 소리: 프레임마다 단음은 첫 목소리만 `nx64_chord(hz, 1, 250)` (= `nx64_tone`), 화음은 모두 `nx64_chord(hz, n, 250)`. 길이를 250 ms 로 주므로 플레이어가 멈추면 커널이 곧 끈다.
  같은 화음이 이어지면 커널은 아르페지오 순서를 처음으로 돌리지 않는다.
  끝나거나 Ctrl-C 로 죽어도 커널이 그 프로세스의 소리를 끈다.
- 설정: `app:/badapple.run/movie` (기본 파일), `app:/badapple.run/ramp`, `app:/badapple.run/sound` (`mono` / `chord`) ([rootfs/app/README.md](../rootfs/app/README.md)).

## PC 스피커 API

사각파 비프음은 이 플레이어만의 것이 아니다:

| 쓰는 곳 | API |
|---|---|
| 커널, 드라이버 | `kernel/timer/pit/pcspk.h`: `pcspk_tone(hz, ms, owner)`, `pcspk_chord(hz[], n, ms, owner)` (기다리지 않음), `pcspk_beep(hz, ms)` (기다림), `pcspk_off()` |
| 시스템 콜 | `11 NxSpeakerTone(hz, ms)`, `12 NxSpeakerChord(u32 *hz, n, ms)` (`include/nyx_abi.h`) |
| 프로그램 (nx64api) | `nx64_tone(hz, ms)`, `nx64_chord(hz, n, ms)`, `nx64_beep(hz, ms)`, `nx64_tone_off()` ([NX64API.md](NX64API.md) 의 "소리") |
| 셸 | `beep -f 880 -l 200`, `beep -c 523,659,784 -l 600` (화음), `beep -f 523 -l 120 -r 3 -d 80`, `beep -s` ([userland/beep/main.c](../userland/beep/main.c)) |
