# Raspberry Pi Camera Client (C++20)

## 목표

Raspberry Pi의 카메라 영상을 JPEG로 캡처해 현재 실행 중인 NestJS 서버의
Socket.IO `/stream` namespace로 전송한다. Raspberry Pi OS Bookworm, C++20,
GNU Make를 기준으로 하며 이후 CI/CD에서 같은 명령을 재사용할 수 있게 구성한다.

최초 설치, systemd 등록, 코드 수정·재배포와 롤백 절차는
[`install.md`](install.md)를 참고한다.

현재 확인한 환경은 다음과 같다.

| 항목 | 확인값 |
| --- | --- |
| Raspberry Pi | `pi@192.168.45.4`, Debian Bookworm arm64 |
| 카메라 | IMX708 Wide, `rpicam-apps` 1.9.0 |
| 서버 | `http://192.168.45.89:3000` |
| 서버 상태 | `/api/health` 정상, vision 서비스 정상 |
| 프로토콜 | Engine.IO 4 + Socket.IO, WebSocket 전용, namespace `/stream` |

## 단계별 작업 계획

각 단계는 구현과 검증이 끝난 뒤 상태와 결과를 이 문서에 갱신한다.

| 단계 | 작업 | 상태 |
| ---: | --- | --- |
| 1 | 서버 계약, Pi 카메라, 컴파일러와 런타임 조사 | 완료 |
| 2 | C++20 프로젝트 구조, Makefile, 설정 파서 작성 | 완료 |
| 3 | `rpicam-vid` 자식 프로세스와 JPEG 프레임 추출 구현 | 완료 |
| 4 | 용량 1 latest-frame 큐와 캡처/전송 스레드 분리 | 완료 |
| 5 | Engine.IO/Socket.IO 연결, 인증, binary event와 ACK 구현 | 완료 |
| 6 | 종료 신호, 재연결 backoff, 통계와 오류 처리 구현 | 완료 |
| 7 | 단위 테스트 및 Pi clean build | 완료 |
| 8 | 실행 서버 연동 및 카메라 목록/프레임 수신 확인 | 완료 |
| 9 | CI/CD용 `make ci` 흐름과 배포 문서 정리 | 완료 |

## 설계

```text
rpicam-vid --codec mjpeg
          │ stdout (연속 JPEG)
          ▼
CameraProcess ──► LatestFrameQueue(capacity=1) ──► SocketIoClient
   캡처 스레드          오래된 프레임 교체            전송 스레드
                                                        │
                                                        ▼
                              http://192.168.45.89:3000/stream
```

- 카메라 캡처는 Raspberry Pi OS가 제공하는 `rpicam-vid`를 자식 프로세스로 실행한다.
- JPEG SOI(`FF D8`)와 EOI(`FF D9`)를 찾아 연속 MJPEG 스트림을 개별 프레임으로 나눈다.
- 네트워크가 느릴 때 큐의 이전 프레임을 최신 프레임으로 교체한다. 영상 지연과 무제한
  메모리 증가를 방지하기 위해 프레임을 누적하거나 재전송하지 않는다.
- 일반 WebSocket 위에 Engine.IO 및 Socket.IO 패킷 형식을 구현한다. Socket.IO 서버에
  일반 WebSocket 메시지만 보내서는 호환되지 않는다.
- 카메라 인증 토큰은 환경 변수 또는 CLI에서 받고 소스, 로그, 저장소에 기록하지 않는다.

## 서버 계약

연결 인증 데이터:

```json
{
  "role": "camera",
  "cameraId": "raspberry-pi-1",
  "token": "CAMERA_TOKEN"
}
```

프레임 이벤트:

```text
event: camera:frame
payload: {
  timestamp: Unix epoch milliseconds,
  frame: JPEG binary
}
```

ACK의 `accepted`가 `false`이고 `reason`이 `backpressure`이면 정상적인 서버 부하
제어이므로 해당 프레임을 버리고 다음 최신 프레임을 보낸다.

## 프로젝트 구조

```text
client/
├── Makefile
├── README.md
├── config/camera-client.env.example
├── include/camera_client/
│   ├── application.hpp
│   ├── camera_process.hpp
│   ├── config.hpp
│   ├── frame.hpp
│   ├── latest_frame_queue.hpp
│   └── socket_io_client.hpp
├── packaging/camera-client.service
├── src/
│   ├── application.cpp
│   ├── camera_process.cpp
│   ├── config.cpp
│   ├── main.cpp
│   └── socket_io_client.cpp
└── tests/test_main.cpp
```

## 코드 리뷰 주석 기준

- 각 공개 함수에는 목적, 입력 인자, 반환값, 실패 조건을 Doxygen 형식으로 설명한다.
- 시스템 호출과 주요 표준/외부 라이브러리 호출에는 성공·실패 반환값을 호출 지점에서
  설명한다.
- 자명한 괄호나 대입문을 반복 설명하기보다 소유권, 수명, 단위, 오류 의미와 프로토콜
  바이트처럼 리뷰 판단에 필요한 내용을 줄 단위 주석으로 남긴다.

## 빌드

Raspberry Pi OS Bookworm에서 필요한 개발 패키지를 설치한다. OpenCV 개발 패키지는
필요하지 않으며 카메라 런타임은 Raspberry Pi OS의 `rpicam-apps`를 사용한다.

```bash
sudo apt-get update
sudo apt-get install -y \
  g++ make libboost-system-dev nlohmann-json3-dev
```

일반 빌드:

```bash
cd ~/workspace/rasberry_pi/client
make
```

빌드 산출물은 `build/camera-client`이다. Makefile은 C++20과 다음 경고를 활성화한다.

```text
-Wall -Wextra -Wpedantic -Wconversion -Wshadow
```

하드웨어가 필요 없는 단위 테스트:

```bash
make test
```

CI와 같은 clean build + test:

```bash
make ci
```

`make ci`는 `clean`, `all`, `test`를 순차적인 서브메이크로 실행한다. 따라서 CI가
병렬 make 옵션을 주더라도 clean과 compile이 서로 경쟁하지 않는다.

## 실행

환경 변수 사용을 권장한다. 실제 토큰을 셸 기록, README 또는 Git에 저장하지 않는다.

```bash
export CAMERA_SERVER_URL=http://192.168.45.89:3000
export CAMERA_TOKEN='서버의-CAMERA_TOKEN'
export CAMERA_ID=raspberry-pi-cpp
./build/camera-client
```

CLI 값은 환경 변수보다 우선한다.

```bash
./build/camera-client \
  --server http://192.168.45.89:3000 \
  --camera-id raspberry-pi-cpp \
  --width 1280 --height 720 --fps 10 --quality 80
```

CLI 전체 목록은 다음 명령으로 확인한다.

```bash
./build/camera-client --help
```

SIGINT(`Ctrl+C`) 또는 SIGTERM을 받으면 signal handler는 atomic 플래그만 변경한다.
일반 실행 흐름이 WebSocket을 닫고 `rpicam-vid`를 종료한 뒤 캡처 스레드를 join한다.

## systemd 설치

```bash
cd ~/workspace/rasberry_pi/client
sudo make install
sudo install -m 0600 config/camera-client.env.example /etc/camera-client.env
sudo editor /etc/camera-client.env
sudo systemctl daemon-reload
sudo systemctl enable --now camera-client
```

서비스 상태와 로그:

```bash
systemctl status camera-client
journalctl -u camera-client -f
```

서비스는 `pi` 사용자와 `video` 보조 그룹으로 실행하며, 실패 시 5초 후 재시작한다.
환경 파일은 반드시 소유자만 읽을 수 있는 `0600` 권한을 유지한다.

## 검증 결과

2026-09-01에 Raspberry Pi 5 / IMX708 Wide에서 다음을 확인했다.

- `g++ 12.2.0`, C++20, arm64에서 `make ci` 성공
- 모든 활성화된 컴파일러 경고 없이 전체 빌드 성공
- 설정 CLI/환경 변수 우선순위, camera ID 검증, latest-frame 교체 단위 테스트 통과
- Engine.IO v4 WebSocket handshake와 Socket.IO `/stream` 카메라 인증 성공
- `camera:frame` JPEG binary event와 서버 ACK 처리 성공
- 20초 실기 테스트: 캡처 189, 서버 승인 158, backpressure 30, 로컬 교체 1
- 추가 정상 종료 테스트: 캡처 69, 서버 승인 58, backpressure 11, 종료 오류 없음
- 서버 누적 확인: 수신 257, 드롭 41, OpenCV 처리 오류 로그 없음
- 테스트 종료 후 `/api/cameras`에서 `connected: false`로 disconnect 반영 확인

`backpressure` 수치는 서버 기본 처리 상한과 영상 처리 시간에 따른 정상 드롭이다.
클라이언트는 거부된 프레임을 재전송하지 않는다.

서버 측 smoke test:

```bash
curl http://SERVER_IP:3000/api/health
curl http://SERVER_IP:3000/api/cameras
```

## 빌드·실행·검증

위 `make ci`, 실제 카메라 실행, 서버의 `/api/health`와 `/api/cameras` 확인을 순서대로
수행한다. 브라우저 영상 확인은 서버의 `http://SERVER_IP:3000`에서 할 수 있다.

## CI/CD 확장 원칙

- `make ci` 한 명령으로 clean build와 단위 테스트를 수행한다.
- `.github/workflows/client-ci.yml`은 Ubuntu runner에서 동일한 `make -C client ci`를
  실행한다.
- 빌드 산출물은 `client/build/`에만 생성하고 Git에 포함하지 않는다.
- 토큰은 CI secret 또는 배포 대상의 권한 제한 환경 파일로 주입한다.
- CI의 일반 Linux runner에서는 하드웨어 없이 단위 테스트를 수행한다.
- Raspberry Pi 배포 단계는 arm64 self-hosted runner 또는 SSH 배포 job으로 분리한다.
- 배포 성공 후 `/api/health`와 `/api/cameras`를 확인하는 smoke test를 둔다.

## 현재 알려진 제약

- 최초 MVP는 현재 서버와 같은 `http://`/`ws://` 연결을 지원한다. 외부망 배포 전에는
  TLS(`https://`/`wss://`) 지원과 서버 인증서 검증을 추가해야 한다.
- 카메라 하드웨어 검증과 실제 영상 확인은 Raspberry Pi에서만 완료로 판단한다.
