# DY-EP4 독립형 Dryer HMI

ESP32-P4 Function EV Board(1024×600)에서 Brookesia Phone Shell 없이 Dryer UI만
부팅 즉시 실행하는 ESP-IDF C++ 프로젝트입니다.

## 빌드

ESP-IDF 5.5 환경에서 다음 명령을 실행합니다.

```powershell
idf.py set-target esp32p4
idf.py -B "$env:TEMP\dy_ep4_build" build
idf.py -B "$env:TEMP\dy_ep4_build" -p COM포트 flash monitor
```

필요한 LVGL 및 보드 BSP는 `components` 폴더에 포함되어 있으므로 원본
`esp_brookesia_phone` 프로젝트를 참조하지 않습니다.

## 주요 코드

- `main/main.cpp`: NVS, LCD/LVGL 초기화 후 Dryer UI 직접 실행
- `components/dryer`: 건조 공정 UI와 상태 머신
- `components/ycb_lcd`: 한글 및 7세그먼트 LVGL 캔버스 렌더러

`DryerApp::readSensors()`는 현재 데모 센서 값을 만듭니다. 실제 센서와 릴레이,
히터, 팬, 댐퍼 출력은 이 함수와 상태 전환부의 TODO 위치에 연결해야 합니다.

Synology Drive가 빌드 중간 파일을 잠글 수 있으므로 위 예시처럼 빌드 디렉터리를
로컬 임시 폴더로 지정하는 것을 권장합니다.
