# 가져온 외부 코드 (THIRD_PARTY)

규칙은 [docs/LICENSING.md](docs/LICENSING.md). 각 디렉터리의 원본 라이선스 파일을 그대로 둔다.

| 디렉터리 | 프로젝트 | 버전 | 라이선스 | 쓰는 곳 | 고친 것 |
|---|---|---|---|---|---|
| `third_party/tinycc` | TinyCC | 0.9.28rc (`third_party/tinycc/VERSION`) | LGPL-2.1-or-later | nlcc (`userland/nlcc`) | 원본 그대로. 고친 것은 `userland/nlcc/tinycc-nyxis.patch` 로 빌드 때 적용 |
| `third_party/kilo` | antirez/kilo | upstream | BSD-2-Clause | kilo (`userland/kilo`) | 원본 그대로. 고친 것은 `userland/kilo/kilo-nyxis.patch` |
| (빌드 때 받음) `media/badapple.mid` | [xqe2011/bad-apple-midi](https://github.com/xqe2011/bad-apple-midi) `input.mid` (원본 [cnlohr/badderapple](https://github.com/cnlohr/badderapple)) | 커밋 `c325dbc6d0209abc986e16df7fcef63a5b4d140c` | 퍼블릭 도메인 (그 저장소의 README). 곡의 저작권은 원작자 | badapple 동영상의 음 트랙 (`make badapple-midi`) | 저장소에 넣지 않는다 |
| `third_party/uzlib` | [pfalcon/uzlib](https://github.com/pfalcon/uzlib) (Joergen Ibsen, Simon Tatham, Paul Sokolovsky) | 커밋 `6d60d651a4499a64f2e5b21b4cc08d98cb84b5c1` (2021-11-15) | Zlib (`third_party/uzlib/LICENSE`) | badapple 플레이어의 inflate (`userland/badapple`), 동영상 인코더의 deflate/inflate (`tools/badapple/nxbwenc.c`), ntui 스크린샷의 deflate (`.ppm.gz`) | 소스는 고치지 않았다. 미리 빌드된 `bin/tgunzip.exe` 만 뺐다 |
