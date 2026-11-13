# UniFrog — tổng hợp thay đổi từ lúc fork

Tài liệu này gom lại toàn bộ thay đổi của fork **hieeeuvn/UniFrog** kể từ commit
đầu tiên (`d3a4591`, 2026-05-04) đến bản mới nhất (`8bf8f36`, 2026-11-13), lấy
trực tiếp từ lịch sử git trên nhánh `main`. Mục đích là để đọc nhanh "đã làm
được gì" mà không phải lội 347 commit.

## Số liệu

| Hạng mục | Giá trị |
| --- | --- |
| Commit trên `main` | 347 (273 trong 05/2026, 55 trong 08/2026, 12 trong 10/2026, 7 trong 11/2026) |
| Dòng thêm / xoá | ~157.900 / ~80.600 |
| Pull request đã merge | 9 |
| Tài liệu trong `docs/` | 38 tệp |
| Bộ test host (`tests/host`) | 11 tệp |
| Workflow CI | 6 (`commit-smoke`, `commit-smoke-full`, `qemu-smoke`, `firmware-build`, `sdcard-zip`, `ra-sync-release`) |
| App pak kèm sẵn | 4 (`artwork-coverage`, `achievements`, `overlay-stats`, `_template`) |

Tác giả: Axel Garcia K. (328 commit), UniFrog CI (18), hieeeu (1).

## Mốc thời gian

**05/2026 — dựng lại nền tảng.** Hợp đồng build mới (`make`, `make quick-check`,
`make verify`), frontend native chạy trên SF2000, JS2300 thành frontend
JavaScript-only (quick menu, luồng quay về launcher bằng JS), thay frontend
shell cũ. Cùng lúc: boot logo sớm trước khi vào UI, chẩn đoán SD mode, audio
startup, gpsp multicore, license bên thứ ba.

**05/2026 (nửa cuối) — khôi phục tính năng native + media.** Dựng lại frontend
native sau giai đoạn JS: theme muOS, font bitmap scale, scheme, layout, media
backend. Đưa FFmpeg upstream vào thay `libffplayer`, giải mã video H264 bằng
hardware (`viddec`) và `auddec` qua KSHM, rồi một chuỗi dài ổn định seek,
pacing, buffering và đường audio GB300 (I2SO / audsink / auddec loop) cho đến
khi chốt được route chạy ổn định. Cuối giai đoạn: tối ưu hot path libretro,
framebuffer RGB565, framebuffer pan.

**08/2026 — tách dependency và ổn định.** JS2300 chuyển sang repository riêng,
port runtime/SDK từ `mufrog-commandc`, QPSX dùng mirror công khai, CI dùng
submodule SDK qua HTTPS. UI: dịch nhãn menu/footer, chữ TTF nét hơn, menu một
cột, box art muOS bật mặc định, cắt dòng theo pixel thay vì theo ký tự, cấp
phát heap cho bảng label/value khi thư viện lớn. Release `260819_1`.

**10–11/2026 — RetroAchievements, app paks, overlay.** Công cụ RA Sync (Android,
Zig) và engine achievements trong firmware, sửa các lỗi đọc RAM GBA qua bộ nhớ
core, rồi mở rộng hệ pak: app pak (một thư mục = một tool), pak xem achievements,
và overlay pak vẽ đè lên game.

## Build, CI và dependency

- `Makefile` là nguồn sự thật duy nhất: `make`, `make quick-check`,
  `make verify`; có `make help` / `make print-config`.
- Dependency core dùng sparse checkout + cache, `ccache` cho core compiler,
  toolchain được cache trong CI; JS2300 không còn vendor trong repo.
- Sửa race build SDK: `deps-sdk` thành order-only prerequisite của
  `$(SDK_KERNEL_LIB)`; bỏ qua bước SDK khi workspace không phải git checkout.
- Workflow: smoke theo commit, smoke đầy đủ, qemu smoke, build firmware theo yêu
  cầu, đóng gói SD ZIP, và release RA Sync.
- Mỗi bản build firmware tạo release nháp chuyển thành prerelease tên
  `fw-<run id>` kèm `UniFrog-firmware.bin`, `UniFrog-firmware-main.zip` (bố cục
  thẻ nhớ đầy đủ), `fastboot.asd` và `SHA256SUMS`.

## Frontend (launcher)

- Runtime C native: danh sách/menu, con trỏ, footer gợi ý, loading có tiến
  trình, thông báo lỗi thay vì màn hình đen.
- Theme muOS: đọc theme, scheme thay thế, font bitmap scale, font TTF/OTF, font
  riêng cho từng theme (`font=` trong `[theme.<name>]`), glyph fallback.
- Ngôn ngữ: dịch nhãn, footer, loading; sửa dấu tiếng Ba Lan và vùng ký tự font.
- ROM artwork: template `muos` / `skraper` / `beside` / `custom` với token
  `{rom_dir}`, `{system}`, `{name}`, `{filename}`; box art bật mặc định; tài liệu
  `docs/game-artwork.md`.
- Hiệu năng: quét thư mục nhanh hơn, khử trùng lặp danh sách bằng hash thay vì
  O(n²), bloom filter cho `tr()`, cache asset theme, cache glyph, chỉ vẽ lại khi
  nội dung đổi.
- Cấu hình `unifrog.ini` dạng `key=value` phẳng: `artwork_*`, `log_level`,
  `storage_*`, hiệu chuẩn pin, `keymap`, `state_*`, `rtc_offset_minutes`,
  `default_boot`, và `overlay_pak`.

## Libretro host và cores

- Tách host / session / policy thành module, quick menu native (thay bản JS),
  fast-forward có điều chỉnh tốc độ và pacing riêng, watchdog cho lời gọi core,
  log `perf_cpu` định kỳ, exception record trước panic.
- Present RGB565 ưu tiên, pan framebuffer rẻ cho core, frameskip, display mode
  fit/stretch/original, GE clock, backlight, audio gain, keymap (RetroArch,
  Genesis, swap AB/XY), save state slot, RTC offset cho game.
- Bộ core: gambatte, snes9x2005/2002, quicknes, fceumm, gearboy, pce-fast,
  frogui, stella2014, a5200, prosystem, atari800, handy, beetle-cygne,
  gearcoleco, gpsp + gpsp_multicore, picodrive, qpsx, bluemsx, fake08, frodo,
  FB Alpha 2012; mỗi core có quirks riêng trong `docs/libretro-core-quirks.md`.

## Trình phát media

- Thay core PMP bằng app media native dùng FFmpeg upstream (có patch tương thích
  HCRTOS), đường decode hardware H264 `viddec` + `auddec`, vòng đệm KSHM.
- Seek theo keyframe, giới hạn catch-up, cache cửa sổ đọc MP4, pacing theo độ
  phân giải, tạm ngưng ghi log trong lúc đọc thẻ để không nghẽn I/O.
- Audio GB300 trải qua nhiều thử nghiệm route (audsink, I2SO, auddec loop,
  mono/mono-direct) trước khi chốt đường ổn định; toàn bộ ghi lại trong
  `docs/sf2000-gb300-audio.md` và `docs/audio-quirks.md`.

## Lưu trữ, SD và phần cứng

- Chẩn đoán SD mode (wide*, hs1, uhs), sweep profile an toàn, chuyển DTB, phục
  hồi khi thẻ lỗi; storage profile chọn lúc chạy kèm profile fallback.
- Giới hạn ghi log xuống thẻ (auto flush theo byte, flush khi pause/đóng game),
  ghi atomic, tránh treo do host-stop.
- Đồng hồ phần mềm bền qua reboot (`clock-speed-resilience`), backlight PWM,
  phát hiện pin yếu (3 mẫu liên tiếp) không đổi SD mode.

## RetroAchievements

- Engine C: hash ROM, cache định nghĩa, đánh giá trigger, toast khi mở khoá,
  danh sách `earned`, đếm achievement không hỗ trợ, chế độ hardcore.
- Sửa các lỗi quan trọng: đọc RAM GBA qua memory map của core (IWRAM/EWRAM rời
  nhau), coi RAM không đọc được là "không biết" thay vì 0, chỉ trao id có trong
  định nghĩa của chính game, trao lại id mà `earned` còn nhớ.
- `tools/ra_sync.mjs` + release RA Sync: login bằng Connect token thay vì Web API
  key, log độ dài key và HTTP status khi lỗi, không đòi hỏi PC.
- Bố cục dữ liệu trong `docs/retroachievements.md`:
  `achievements/cache/<hash>.json`, `achievements/earned/<hash>.txt`,
  `achievements/queue/unlocks.jsonl`, `achievements/ra.ini`.

## App paks (2026-11-13)

- Một pak là một thư mục trong `/unifrog_data/apps/<id>/` với `app.ini`
  (`title`, `info`, `icon`, `entry`) và script JS2300; launcher chỉ đọc manifest
  rồi chuyển cho script launcher có sẵn, nên thêm tool không cần build lại
  firmware. Entry bị chặn `..`, `\`, `:`, đường dẫn tuyệt đối.
- Icon pak có bảng 24 slot, fallback về glyph theme khi hết slot.
- Gói SD và `tools/install-sdcard.sh` tự copy `paks/` xuống thẻ.
- Pak kèm sẵn: `artwork-coverage` (kiểm tra box art, ghi report),
  `achievements` (xem tiến độ mọi game đã cache), `_template`, và
  `overlay-stats` (overlay pak).
- Overlay pak (`overlay.ini`): `position` + `line1..line4` với token `clock`,
  `battery`, `achievements`, `points`, `game`; firmware vẽ native lên frame game
  sau mỗi frame và chỉ flush vùng panel. Bật bằng **Config → Custom → Game
  Overlay**, lưu thành `overlay_pak` trong `unifrog.ini`.
- Tài liệu: `docs/app-paks.md`.

## Tài liệu và kiểm chứng

- 38 tệp trong `docs/`, từ kiến trúc (`unifrog-architecture.md`), bản đồ sở hữu
  (`ownership-map.md`), tới phần cứng (`sf2000-hardware-findings.md`,
  `sf2000-memory-model.md`, `sf2000-mmc-driver.md`).
- 11 bộ test host: achievements, associations, config, diagnostic policy,
  foundation, frontend controller / rom roots / services, libretro policy, media
  policy.
- Kiểm tra không cần thiết bị: `make quick-check`, `frontend-structure-check`,
  `architecture-check`, `host-frontend-check` (dựng model UI và so snapshot),
  `host-visual-check`, `host-sanitize-check`, và qemu smoke.

## Bản phát hành gần đây

| Tag | Tên | Ghi chú |
| --- | --- | --- |
| `fw-43373057309` | pak: achievements viewer | 2026-11-13, `UniFrog-firmware.bin` sha256 `cf8740b9…72fac` |
| `fw-43371976606` | app paks: tools as SD folders | 2026-11-13, sha256 `0e6ee9d8…f0725` |
| `fw-43206973514` | — | 2026-11-12, sha256 `b2c980f9…55b8b` |

## Còn treo

- Cài pak trực tiếp từ `.zip` trong `unifrog_data/archive` (không cần máy tính).
- Pak giải nén artwork pack ZIP vào catalogue.
- Auto save đang bị comment out.
- Chưa có kiểm thử trên thiết bị thật trong CI: overlay pak và pak achievements
  mới chỉ được kiểm bằng cross-compile, harness Node và build firmware.
- RA Sync trên Android cần người dùng tự nhập khoá; chưa có luồng đăng nhập
  trong app.
