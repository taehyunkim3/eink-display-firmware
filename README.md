# ESP32 E-ink Firmware

PlatformIO 기반 ESP32 펌웨어입니다. `eink-frontend`의 `/api/screen.json` 데이터를 받아 ESP32가 800 x 480 e-ink 화면을 직접 그립니다.

## 1. 설정

```bash
cd /Users/user/Documents/GithubFolder/PERSONAL/eink/eink-firmware
cp include/config.example.h include/config.h
```

`include/config.h`에서 아래 값을 채웁니다.

```cpp
#define WIFI_SSID "..."
#define WIFI_PASSWORD "..."
#define DEVICE_ENDPOINT "https://your-app.vercel.app/api/screen.json"
#define DEVICE_AUTH_TOKEN "eink-frontend의 DEVICE_AUTH_TOKEN과 같은 값"
```

로컬 Next.js 서버로 테스트할 때는 `localhost`가 아니라 Mac의 같은 Wi-Fi LAN IP를 써야 합니다.

```cpp
#define DEVICE_ENDPOINT "http://192.168.0.10:3000/api/screen.json"
```

## 2. 보드 연결 확인

현재 Mac에서는 보드가 아래 포트로 보였습니다.

```bash
/dev/cu.usbserial-110
```

포트가 달라지면 `platformio.ini`의 `upload_port`, `monitor_port`를 바꿉니다.

## 3. 업로드

PlatformIO가 없다면 둘 중 하나로 설치합니다.

```bash
python3 -m pip install --user platformio
```

또는 VS Code에서 `PlatformIO IDE` 확장을 설치해도 됩니다.

CLI 업로드:

```bash
pio run -t upload
pio device monitor
```

현재 Mac에서는 `pio`가 `/Users/user/Library/Python/3.9/bin/pio`에 설치됐습니다. PATH에 없으면 아래처럼 실행합니다.

```bash
/Users/user/Library/Python/3.9/bin/pio run -t upload
/Users/user/Library/Python/3.9/bin/pio device monitor
```

VS Code에서는 PlatformIO 탭에서 `Upload`, `Monitor`를 실행합니다.

## 4. 첫 테스트 순서

1. `eink-frontend`를 Vercel에 배포하거나 로컬에서 `npm run dev`로 실행합니다.
2. `curl`로 JSON API가 200을 주는지 확인합니다.
   ```bash
   curl -H "Authorization: Bearer <token>" -I https://your-app.vercel.app/api/screen.json
   ```
3. `include/config.h`에 같은 URL과 토큰을 넣습니다.
4. USB로 보드를 연결하고 업로드합니다.
5. Serial Monitor에서 `HTTP failed`, `Bitmap download failed`, `Bitmap size mismatch`, `Render failed` 중 어디서 막히는지 확인합니다.

## 5. 버튼 조작

reTerminal E1001 상단 3개 버튼은 이벤트 방식으로 처리합니다. 버튼을 누른 순간 바로 명령을 실행하지 않고, debounce 후 버튼을 뗄 때 짧은 클릭 이벤트를 만듭니다. 좌/우 동시 길게 누르기는 별도 hold 이벤트로 처리해서, Wi-Fi 설정을 잘 잡으면서도 일반 페이지 이동이 느려지지 않게 했습니다.

메인 화면:

- 왼쪽 짧게: 이전 페이지
- 오른쪽 짧게: 다음 페이지
- refresh 짧게: 현재 페이지를 서버 강제 갱신
- 왼쪽 + 오른쪽 길게: 기기 설정 진입

기기 설정 화면:

- 왼쪽/오른쪽 짧게: `와이파이 설정` / `화면 설정` 메뉴 이동
- refresh 짧게: 선택한 메뉴로 들어가기
- 왼쪽 + 오른쪽 길게: 저장하지 않고 나가기

와이파이 설정:

- 왼쪽 짧게: 목록 위 / 문자 이전
- 오른쪽 짧게: 목록 아래 / 문자 다음
- refresh 짧게: 선택 / 현재 문자 입력
- 같은 문자에서 refresh를 빠르게 두 번: 비밀번호 저장
- 왼쪽 + 오른쪽 길게: 저장하지 않고 나가기

화면 설정:

- 왼쪽/오른쪽 짧게: 설정 항목 이동
- `전체 refresh`: 몇 번의 화면 전환마다 전체 refresh 할지 변경
- `웹 데이터 갱신`: 서버 데이터를 몇 분/시간마다 새로 받을지 변경
- `저장하고 나가기`: 설정을 ESP32 Preferences에 저장

관련 설정값:

```cpp
#define BUTTON_DEBOUNCE_MS 30
#define BUTTON_SCAN_INTERVAL_MS 10
#define BUTTON_CLICK_MIN_MS 30
#define WIFI_SETUP_CHORD_GRACE_MS 700
#define WIFI_SETUP_HOLD_MS 1500
#define PAGE_FULL_REFRESH_INTERVAL 5
#define FALLBACK_SLEEP_SECONDS 1800
```

`BUTTON_CLICK_MIN_MS`는 노이즈성 순간 입력을 클릭으로 보지 않기 위한 최소 눌림 시간입니다. `WIFI_SETUP_CHORD_GRACE_MS`는 좌/우를 완전히 동시에 누르지 못해도 chord로 인정하는 최대 시차입니다. `WIFI_SETUP_HOLD_MS`는 두 버튼이 함께 눌린 뒤 설정 메뉴로 들어가기까지 유지해야 하는 시간입니다. 단일 클릭 반응이 느리면 hold 시간을 줄이는 게 아니라, Serial Monitor에서 `Button: page left`, `Button: page right`, `Button: settings chord hold` 로그가 어떻게 찍히는지 먼저 확인합니다.

`PAGE_FULL_REFRESH_INTERVAL`과 `FALLBACK_SLEEP_SECONDS`는 기본값입니다. 기기 설정의 화면 설정에서 바꾼 값은 ESP32 Preferences에 저장되고, 다음 부팅 후에도 유지됩니다. 화면 전환 카운터는 메인 페이지, 설정 메뉴, 설정 하위 화면 전환에서 공통으로 사용됩니다.

## 6. SD 카드 자산

reTerminal E1001의 microSD는 FAT32, 최대 32GB를 사용합니다. 펌웨어는 부팅 시 SD를 마운트하고 아래 경로의 1-bit PBM 아이콘을 읽습니다.

```text
/eink/icons/weather_sun.pbm
/eink/icons/weather_cloud.pbm
/eink/icons/weather_rain.pbm
```

파일이 없으면 기본 아이콘을 SD 카드에 자동 생성합니다. 같은 파일명으로 32x32 `P4` PBM 파일을 교체하면 펌웨어 재빌드 없이 날씨 아이콘을 바꿀 수 있습니다.

## 7. 화면이 안 나오면 먼저 볼 것

- `EPD_MODEL`: 기본값은 `GxEPD2_750_GDEY075T7`입니다. 컴파일 오류가 나면 `GxEPD2_750_T7`을 시도합니다.
- `EPD_*` 핀: 제품 위키/회로도와 다르면 `include/config.h`에서 바꿉니다.
- `DEVICE_ENDPOINT`: ESP32에서 접근 가능한 URL이어야 합니다. 로컬 테스트의 `localhost`는 ESP32 자기 자신을 의미하므로 안 됩니다.
- `DEVICE_AUTH_TOKEN`: `eink-frontend`의 `DEVICE_AUTH_TOKEN`과 정확히 같아야 합니다.
- JSON 크기: Serial Monitor에 찍히는 `JSON content length`가 `MAX_JSON_BYTES`보다 작은지 확인합니다.
- `err=-11` 또는 음수 HTTP 에러가 반복되면 Wi-Fi/TLS/타임아웃 계열입니다. `DEVICE_FETCH_ATTEMPTS`, `DEVICE_HTTP_CONNECT_TIMEOUT_MS`, `DEVICE_HTTP_TIMEOUT_MS` 값을 조금 늘립니다.

## 8. 절전

첫 업로드 때는 `ENABLE_DEEP_SLEEP false`가 편합니다. 화면 갱신이 확인되면 아래처럼 바꿉니다.

```cpp
#define ENABLE_DEEP_SLEEP true
#define FALLBACK_SLEEP_SECONDS 1800
```

e-ink는 10분에서 1시간 주기 갱신이 적당합니다.
