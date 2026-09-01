# Raspberry Pi C++ Camera Sender — AI 개발 지침

이 문서는 AI 코딩 에이전트가 Raspberry Pi용 C++ 카메라 송신기를 개발할 때 따라야 하는 프로젝트 지침이다. 구현 전에 반드시 서버 계약인 [`../README.md`](../README.md)와 현재 Python 참조 구현인 [`sender.py`](sender.py)를 함께 읽는다.

## 1. 목표

Raspberry Pi Camera Module에서 프레임을 캡처하고 JPEG로 인코딩한 다음, Camera real-time viewer 서버의 Socket.IO `/stream` namespace로 전송한다.

완성 기준:

- Raspberry Pi OS Bookworm 64-bit에서 빌드하고 실행할 수 있다.
- libcamera 기반으로 프레임을 캡처한다.
- JPEG를 Base64 문자열이 아닌 binary로 전송한다.
- 서버가 느릴 때 프레임을 쌓지 않고 버려 실시간성을 유지한다.
- 토큰이나 서버 주소를 소스 코드에 저장하지 않는다.
- SIGINT와 SIGTERM에서 카메라, 소켓, 스레드를 정상적으로 정리한다.
- 장시간 실행과 네트워크 재연결을 지원한다.

## 2. 서버 연결 계약

서버 기본 주소는 `http://SERVER_IP:3000`, Socket.IO namespace는 `/stream`, transport는 WebSocket이다.

연결 시 다음 auth 값을 전달한다.

```json
{
  "role": "camera",
  "cameraId": "raspberry-pi-1",
  "token": "CAMERA_TOKEN 값"
}
```

`cameraId` 규칙:

- 영문자, 숫자, `_`, `-`만 사용한다.
- 길이는 1~64자이다.
- 배포 장치마다 고유하고 지속적인 값을 사용한다.

프레임은 `camera:frame` 이벤트로 전송한다.

```text
{
  timestamp: Unix epoch milliseconds (integer),
  frame: JPEG binary
}
```

서버 ACK 형식:

```json
{ "accepted": true }
```

또는:

```json
{ "accepted": false, "reason": "backpressure" }
```

가능한 실패 사유는 `unauthorized`, `invalid_jpeg`, `backpressure`, `processing_failed`이다.

- `backpressure`: 정상적인 과부하 제어다. 해당 프레임을 버리고 다음 프레임을 전송한다.
- `unauthorized`: 토큰 또는 연결 role을 확인하고 무한 재시도하지 않는다.
- `invalid_jpeg`: JPEG 인코딩과 최대 크기를 확인한다.
- `processing_failed`: 지수 백오프를 적용하되 캡처 루프를 막지 않는다.

현재 서버의 프레임 제한은 기본 2 MiB, 처리 상한은 카메라당 12 FPS다. 기본 송신값은 1280×720, 10 FPS, JPEG quality 80으로 한다.

## 3. CAMERA_TOKEN의 의미와 취급

`CAMERA_TOKEN`은 영상 암호화 키가 아니라 **카메라 송신 장치 인증용 공유 비밀값**이다. 서버의 `CAMERA_TOKEN`과 Pi가 연결 시 보내는 token이 같아야 한다.

반드시 지킬 사항:

- 토큰을 `.cpp`, `.hpp`, CMake 파일, Git 저장소에 하드코딩하지 않는다.
- CLI의 `--token`을 지원하더라도 운영 환경에서는 `CAMERA_TOKEN` 환경 변수를 우선 권장한다.
- systemd에서는 권한이 `0600`인 `/etc/camera-viewer.env`를 사용한다.
- 로그, 예외 메시지, 연결 URL에 토큰을 출력하지 않는다.
- 인터넷에 노출할 때 토큰만 믿지 말고 TLS(HTTPS/WSS)를 적용한다.
- 장치가 여러 대라면 최종 운영 버전에서 장치별 토큰 또는 mTLS로 확장한다.

개발 서버의 현재 임시 토큰 `test-token`은 로컬 테스트 전용이다. 운영 배포 전에 충분히 긴 무작위 값으로 변경한다.

예시:

```bash
export CAMERA_TOKEN='replace-with-a-long-random-value'
./camera-sender --server http://192.168.0.10:3000 --camera-id raspberry-pi-1
```

## 4. 권장 C++ 기술 선택

- 언어 표준: C++20
- 빌드: CMake 3.25 이상
- 카메라: `libcamera` C++ API
- 영상 처리/JPEG: OpenCV 4 (`cv::imencode`)
- Socket.IO: `socket.io-client-cpp`
- 설정/CLI: 환경 변수 + 간단한 명령행 파서
- 로깅: 초기 버전은 thread-safe 표준 출력, 확장 시 `spdlog`
- 테스트: Catch2 또는 GoogleTest 중 하나만 선택

`socket.io-client-cpp`는 서버가 사용하는 Socket.IO 프로토콜과 binary event를 맞추기 위한 선택이다. 일반 WebSocket 라이브러리만으로 Socket.IO 서버에 직접 연결하면 안 된다. Socket.IO는 WebSocket 위에 별도 handshake와 event framing을 사용한다.

의존성을 새로 추가할 때는 다음 순서로 판단한다.

1. Raspberry Pi OS 패키지로 설치 가능한가?
2. arm64에서 지속적으로 유지보수되는가?
3. 정적/동적 링크와 라이선스가 배포 방식에 맞는가?
4. 더 작은 의존성으로 같은 요구 사항을 충족할 수 있는가?

## 5. 권장 프로젝트 구조

```text
raspberry-pi/cpp-sender/
├── CMakeLists.txt
├── CMakePresets.json
├── README.md
├── config/
│   └── camera-viewer.env.example
├── include/camera_sender/
│   ├── application.hpp
│   ├── camera_capture.hpp
│   ├── config.hpp
│   ├── frame.hpp
│   └── socket_client.hpp
├── src/
│   ├── application.cpp
│   ├── camera_capture.cpp
│   ├── config.cpp
│   ├── main.cpp
│   └── socket_client.cpp
├── tests/
│   ├── config_test.cpp
│   └── frame_queue_test.cpp
└── packaging/
    └── camera-viewer-cpp.service
```

한 파일에 카메라, 네트워크, 설정, 재연결 로직을 모두 넣지 않는다.

## 6. 필수 설계 규칙

### 캡처와 전송 분리

- 캡처 스레드와 네트워크 전송 스레드를 분리한다.
- 두 스레드 사이에는 크기 1 또는 2의 bounded latest-frame queue를 둔다.
- 큐가 가득 차면 오래된 프레임을 버리고 최신 프레임으로 교체한다.
- 네트워크 장애가 캡처 장치를 멈추거나 메모리를 증가시키면 안 된다.

### 프레임 수명과 복사

- 카메라 버퍼의 수명이 끝난 뒤 참조하지 않는다.
- 소켓 라이브러리가 비동기로 데이터를 사용하는 경우 전송 완료까지 JPEG buffer의 소유권을 유지한다.
- 불필요한 RGB 전체 프레임 복사를 줄이되, 안전하지 않은 zero-copy 최적화를 먼저 구현하지 않는다.

### 시간

- `timestamp`는 `std::chrono::system_clock` 기반 Unix epoch milliseconds다.
- FPS 제어와 재연결 대기는 `std::chrono::steady_clock`을 사용한다.
- 시스템 시간 역행이 전송 주기 계산에 영향을 주지 않게 한다.

### 재연결

- 최초 간격 1초, 최대 30초의 exponential backoff와 작은 jitter를 사용한다.
- 연결 성공 시 backoff를 초기화한다.
- 인증 실패는 설정 오류로 분류하고 빠른 무한 재연결을 하지 않는다.
- 재연결 중에도 latest-frame queue 크기는 제한한다.

### 종료

- signal handler에서는 atomic 종료 플래그만 변경한다.
- 실제 카메라 중지, socket close, thread join은 정상 실행 흐름에서 수행한다.
- detached thread를 만들지 않는다.
- RAII로 카메라와 네트워크 리소스를 관리한다.

## 7. 설정 우선순위

설정은 다음 우선순위를 따른다.

1. 명령행 옵션
2. 환경 변수
3. 안전한 기본값

지원해야 할 값:

| CLI | 환경 변수 | 기본값 |
|---|---|---:|
| `--server` | `CAMERA_SERVER_URL` | 필수 |
| `--token` | `CAMERA_TOKEN` | 필수 |
| `--camera-id` | `CAMERA_ID` | `raspberry-pi-1` |
| `--width` | `CAMERA_WIDTH` | `1280` |
| `--height` | `CAMERA_HEIGHT` | `720` |
| `--fps` | `CAMERA_FPS` | `10` |
| `--quality` | `JPEG_QUALITY` | `80` |

검증 범위:

- width/height는 양수이며 카메라가 지원하는 해상도여야 한다.
- FPS는 1~30, JPEG quality는 30~95로 제한한다.
- server URL은 `http://` 또는 `https://`만 허용한다.
- 필수 설정이 없으면 토큰 값을 노출하지 않는 명확한 오류와 함께 종료한다.

## 8. 구현 순서

AI 에이전트는 한 번에 전체 코드를 생성하지 말고 다음 단계별로 구현하고 매 단계 검증한다.

1. CMake 프로젝트와 설정 파서
2. libcamera 장치 열기와 단일 프레임 캡처
3. OpenCV JPEG 인코딩과 JPEG magic byte 검사
4. latest-frame bounded queue와 단위 테스트
5. Socket.IO 연결/auth 및 작은 고정 JPEG 전송
6. 캡처·인코딩·전송 파이프라인 통합
7. ACK 처리, 드롭 통계, 재연결/backoff
8. SIGINT/SIGTERM 정상 종료
9. systemd 서비스와 설치 문서
10. 30분 이상 soak test와 메모리/온도/CPU 확인

각 단계가 끝날 때 빌드 명령, 테스트 결과, 실제 Pi 확인 여부를 README에 기록한다.

## 9. 테스트 요구 사항

최소 단위 테스트:

- 환경 변수/CLI 우선순위
- 필수 설정 누락과 범위 오류
- bounded queue가 최신 프레임만 보존하는지
- epoch millisecond 생성
- 재연결 backoff 상한
- ACK reason 분류

통합 테스트:

1. 이 저장소에서 서버를 실행한다.

   ```bash
   cd server
   CAMERA_TOKEN=test-token docker compose up -d
   ```

2. Pi 송신기를 실행하고 `http://SERVER_IP:3000`에서 영상을 확인한다.
3. 서버의 `/api/cameras`에서 해당 `cameraId`와 frame 통계를 확인한다.
4. 서버를 20초 중지했다 다시 시작해 자동 재연결을 확인한다.
5. Pi 송신 FPS를 서버 상한보다 높여 메모리 증가 없이 drop되는지 확인한다.
6. 잘못된 토큰이 거부되고 토큰이 로그에 나타나지 않는지 확인한다.

성능 관찰값:

- 실제 캡처 FPS와 전송 FPS
- JPEG 평균/최대 크기
- ACK accepted/drop 수
- 재연결 횟수
- 프로세스 RSS
- Pi CPU 사용률과 온도

## 10. 빌드 및 품질 기준

권장 컴파일 옵션:

```cmake
target_compile_features(camera-sender PRIVATE cxx_std_20)
target_compile_options(camera-sender PRIVATE
  $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Wconversion -Wshadow>
)
```

개발 빌드에서는 AddressSanitizer와 UndefinedBehaviorSanitizer 옵션을 제공한다. Raspberry Pi 운영 빌드에서는 성능을 측정한 뒤 `Release`를 사용한다.

코드 규칙:

- 소유권은 값, `std::unique_ptr`, `std::shared_ptr` 순으로 최소화한다.
- raw owning pointer와 수동 `new`/`delete`를 사용하지 않는다.
- 예외를 스레드 경계 밖으로 전파하지 않는다.
- 라이브러리 호출 결과를 무시하지 않는다.
- 토큰, 전체 프레임 데이터, 민감한 URL query를 로그로 남기지 않는다.
- 자동 포맷은 `.clang-format`, 정적 분석은 `clang-tidy`를 사용한다.

## 11. AI 에이전트 작업 원칙

- Raspberry Pi OS/arm64에서 존재하지 않는 패키지나 API를 추측하지 말고 설치 가능 여부를 확인한다.
- `libcamera` 버전에 따른 API 차이를 발견하면 대상 OS 이미지와 패키지 버전을 README에 명시한다.
- 서버 프로토콜을 임의로 변경하지 않는다. 변경이 필요하면 서버와 Pi 코드, 문서를 같은 작업에서 함께 수정한다.
- 기존 Python 송신기는 동작 비교용 reference로 유지한다.
- 실제 Raspberry Pi 하드웨어에서 확인하지 않은 기능은 완료라고 표현하지 않는다.
- 네트워크 실패와 느린 서버를 정상적인 상태로 취급하고 메모리 제한과 종료 가능성을 보장한다.
- 빌드 산출물, 토큰 파일, IDE 캐시는 Git에 추가하지 않는다.
- 구현 후 실행 방법, 알려진 제약, 검증 결과를 `cpp-sender/README.md`에 남긴다.

## 12. 완료 정의

다음 항목을 모두 만족해야 C++ 송신기 MVP가 완료된 것이다.

- Pi에서 clean build 성공
- 올바른 토큰으로 서버 연결 성공
- 잘못된 토큰으로 연결 거부 확인
- 브라우저에서 최소 10분 연속 영상 확인
- 서버 재시작 후 자동 재연결
- 네트워크 단절 중 메모리의 지속 증가 없음
- Ctrl+C와 systemd stop에서 5초 이내 정상 종료
- 토큰이 Git과 로그에 없음
- 단위 테스트와 사용 문서 포함
