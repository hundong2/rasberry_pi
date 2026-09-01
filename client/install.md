# C++ Camera Client 설치·수정·배포 설명서

## 1. 문서 범위

이 문서는 Raspberry Pi에서 C++20 카메라 클라이언트를 처음 빌드하는 과정부터 수동
연동 시험, systemd 상시 서비스 등록, 설정 변경, 코드 수정, 재배포, 롤백, 제거와 장애
확인까지 설명한다.

기준 경로와 구성은 다음과 같다.

| 항목 | 경로 또는 값 |
| --- | --- |
| Raspberry Pi 소스 | `/home/pi/workspace/rasberry_pi/client` |
| 빌드 결과 | `client/build/camera-client` |
| 운영 바이너리 | `/opt/camera-client/bin/camera-client` |
| systemd unit | `/etc/systemd/system/camera-client.service` |
| 운영 환경 파일 | `/etc/camera-client.env` |
| 서비스 사용자 | `pi` (`video` 보조 그룹 포함) |
| 예제 서버 | `http://192.168.45.89:3000` |

운영 환경의 IP, 사용자와 저장소 경로가 다르면 모든 명령을 실제 값에 맞게 바꾼다.

## 2. 동작 구조

```text
Raspberry Pi IMX708
       │
       ▼
rpicam-vid (MJPEG stdout)
       │
       ▼
camera-client ── Engine.IO 4 / Socket.IO binary ──► NestJS /stream
       │                                                │
       └─ systemd가 시작·중지·재시작·로그 관리          ▼
                                                   웹 브라우저
```

카메라 클라이언트가 실행 중이어야 서버 화면이 실시간으로 갱신된다. 웹 화면과 서버가
정상이더라도 `camera-client`가 중지되어 있으면 `/api/cameras`에는
`"connected": false`가 표시되고 새 프레임이 오지 않는다.

## 3. 사전 요구 사항

### 3.1 운영체제와 네트워크

- Raspberry Pi OS Bookworm 64-bit
- 서버와 Raspberry Pi가 서로 접근 가능한 네트워크
- Pi에서 서버의 TCP 3000 포트 접근 가능
- 서버와 Pi의 시간이 정상적으로 동기화된 상태

Pi에서 서버 상태를 먼저 확인한다.

```bash
curl --fail --show-error http://192.168.45.89:3000/api/health
```

정상이면 HTTP 200과 `"status":"ok"`가 포함된 JSON이 출력된다. 실패하면 클라이언트
설치보다 서버 주소, 방화벽, Docker 포트 공개와 동일 네트워크 연결을 먼저 해결한다.
Pi에서 `127.0.0.1` 또는 `localhost`는 Pi 자신을 가리키므로, Mac에서 실행 중인 서버에
접속할 때는 Mac의 LAN IP를 사용해야 한다.

### 3.2 카메라 확인

```bash
rpicam-hello --list-cameras
rpicam-hello --timeout 5000
```

첫 명령에는 카메라 모델과 지원 모드가 나와야 한다. 두 번째 명령은 약 5초간 미리보기를
실행한다. SSH처럼 화면이 없는 환경에서는 다음 캡처 명령으로 대신 확인할 수 있다.

```bash
rpicam-jpeg --nopreview --timeout 1000 --output /tmp/camera-test.jpg
file /tmp/camera-test.jpg
```

확인이 끝난 임시 이미지는 삭제해도 된다.

### 3.3 사용자 권한 확인

```bash
id pi
```

출력 그룹에 `video`가 없으면 추가하고 다시 로그인한다.

```bash
sudo usermod --append --groups video pi
```

systemd unit에도 `SupplementaryGroups=video`가 선언되어 있다.

## 4. 빌드 의존성 설치

```bash
sudo apt-get update
sudo apt-get install --yes \
  g++ \
  make \
  libboost-system-dev \
  nlohmann-json3-dev
```

각 의존성의 용도는 다음과 같다.

| 패키지 | 용도 |
| --- | --- |
| `g++` | C++20 컴파일러 및 표준 라이브러리 |
| `make` | 반복 가능한 빌드·테스트·설치 타깃 실행 |
| `libboost-system-dev` | Boost.Asio/Beast WebSocket 네트워크 계층 |
| `nlohmann-json3-dev` | Socket.IO 인증, 이벤트와 ACK JSON 처리 |

카메라 캡처는 Raspberry Pi OS의 `rpicam-vid`를 사용한다. 다음 명령이 실패하면
`rpicam-apps`가 설치된 Raspberry Pi OS 이미지인지 확인한다.

```bash
command -v rpicam-vid
rpicam-vid --version
```

## 5. 소스 준비

이미 저장소가 있다면 최신 `main`을 가져온다. 로컬 변경이 있을 때 `git pull`로 덮어쓰지
말고 먼저 `git status`를 확인한다.

```bash
cd /home/pi/workspace/rasberry_pi
git status --short --branch
git pull --ff-only origin main
cd client
```

처음 받는 장치라면 원하는 작업 디렉터리에 저장소를 clone한 뒤 `client`로 이동한다.

```bash
mkdir -p /home/pi/workspace
cd /home/pi/workspace
git clone https://github.com/hundong2/rasberry_pi.git
cd rasberry_pi/client
```

빌드에 필요한 주요 파일을 확인한다.

```bash
test -f Makefile
test -f packaging/camera-client.service
test -f config/camera-client.env.example
```

각 `test` 명령은 파일이 있으면 종료 코드 0, 없으면 1을 반환한다.

## 6. 최초 빌드와 테스트

### 6.1 Make 타깃

```bash
make help
```

| 명령 | 동작 |
| --- | --- |
| `make` 또는 `make all` | `build/camera-client` 빌드 |
| `make test` | 하드웨어 독립 단위 테스트 빌드 및 실행 |
| `make ci` | clean → 전체 빌드 → 단위 테스트 순차 실행 |
| `make clean` | `client/build/`만 제거 |
| `sudo make install` | 운영 바이너리와 systemd unit 설치 |
| `sudo make uninstall` | 설치된 바이너리와 unit 제거 |

### 6.2 Clean build

배포 전에는 반드시 CI와 동일한 명령을 사용한다.

```bash
cd /home/pi/workspace/rasberry_pi/client
make ci
```

성공 조건:

- 모든 컴파일 명령에 `-std=c++20`이 포함된다.
- `-Wall -Wextra -Wpedantic -Wconversion -Wshadow` 경고가 발생하지 않는다.
- 마지막에 `All camera client tests passed`가 출력된다.
- 다음 파일이 실행 가능한 상태로 생성된다.

```bash
test -x build/camera-client
./build/camera-client --help
```

`make ci`가 실패하면 운영 서비스를 중지하거나 설치하지 않는다. 이미 설치된
`/opt/camera-client/bin/camera-client`에는 영향을 주지 않으므로 기존 서비스는 계속
실행할 수 있다.

## 7. 운영 설정값 준비

### 7.1 설정 우선순위

클라이언트 설정 우선순위는 다음과 같다.

1. CLI 옵션
2. 환경 변수
3. 코드 기본값

운영에서는 토큰이 프로세스 목록이나 셸 기록에 남지 않도록 `--token` 대신 권한이
제한된 systemd 환경 파일을 사용한다.

| 환경 변수 | 기본값 | 설명 |
| --- | --- | --- |
| `CAMERA_SERVER_URL` | 없음 | `http://서버_IP:포트`, 필수 |
| `CAMERA_TOKEN` | 없음 | 서버 `CAMERA_TOKEN`과 동일한 값, 필수 |
| `CAMERA_ID` | `raspberry-pi-1` | 영문자·숫자·`_`·`-`, 장치별 고유값 |
| `CAMERA_WIDTH` | `1280` | 캡처 너비, 양수 |
| `CAMERA_HEIGHT` | `720` | 캡처 높이, 양수 |
| `CAMERA_FPS` | `10` | 1~30 |
| `JPEG_QUALITY` | `80` | 30~95 |

현재 MVP는 `http://`/`ws://`만 지원한다. 외부망에 공개하기 전에는 코드에 TLS와 서버
인증서 검증을 추가해야 하며, 토큰만으로 인터넷 구간을 보호하면 안 된다.

### 7.2 수동 실행용 설정

최초 설치 전에는 현재 셸에만 값을 넣어 연결을 확인한다. 실제 토큰을 README, Git,
채팅, 터미널 스크린샷 또는 배포 로그에 남기지 않는다.

```bash
export CAMERA_SERVER_URL=http://192.168.45.89:3000
export CAMERA_TOKEN='서버와 동일한 실제 토큰'
export CAMERA_ID=raspberry-pi-cpp
export CAMERA_WIDTH=1280
export CAMERA_HEIGHT=720
export CAMERA_FPS=10
export JPEG_QUALITY=80
```

토큰 값을 출력하는 `echo`, `printenv CAMERA_TOKEN`, `set -x`는 사용하지 않는다.

## 8. systemd 등록 전 수동 연동 시험

```bash
cd /home/pi/workspace/rasberry_pi/client
./build/camera-client
```

정상 로그에는 다음 흐름이 나타난다.

```text
[INFO] camera capture started for raspberry-pi-cpp
[INFO] connecting to http://192.168.45.89:3000
[INFO] Socket.IO /stream authentication succeeded
```

다른 터미널에서 서버 상태를 확인한다.

```bash
curl --fail --show-error http://192.168.45.89:3000/api/cameras
```

확인할 값:

- 대상 `cameraId`가 존재한다.
- `connected`가 `true`이다.
- 여러 번 조회할 때 `receivedFrames`가 증가한다.
- 브라우저에서 해당 카메라를 선택하면 영상이 계속 갱신된다.

시험 종료는 클라이언트 터미널에서 `Ctrl+C`를 누른다. 클라이언트는 WebSocket과
`rpicam-vid`를 정리하고 종료한다. 종료 후 API의 `connected`가 `false`로 바뀌는 것은
정상이다.

## 9. systemd 설치 및 상시 실행

### 9.1 바이너리와 unit 설치

```bash
cd /home/pi/workspace/rasberry_pi/client
sudo make install
```

설치 결과를 확인한다.

```bash
sudo test -x /opt/camera-client/bin/camera-client
sudo test -f /etc/systemd/system/camera-client.service
sudo systemd-analyze verify /etc/systemd/system/camera-client.service
```

### 9.2 운영 환경 파일 생성

예제 파일을 복사한 뒤 root만 읽을 수 있게 한다.

```bash
sudo install \
  --owner=root \
  --group=root \
  --mode=0600 \
  config/camera-client.env.example \
  /etc/camera-client.env
sudoedit /etc/camera-client.env
```

파일 예시:

```dotenv
CAMERA_SERVER_URL=http://192.168.45.89:3000
CAMERA_TOKEN=서버와-동일한-실제-토큰
CAMERA_ID=raspberry-pi-cpp
CAMERA_WIDTH=1280
CAMERA_HEIGHT=720
CAMERA_FPS=10
JPEG_QUALITY=80
```

토큰 문자열에 공백이나 `#` 같은 특수문자가 있으면 systemd EnvironmentFile 문법에 맞게
큰따옴표로 감싼다. 파일 권한과 소유자를 다시 확인하되 내용은 출력하지 않는다.

```bash
sudo stat --format='%U %G %a %n' /etc/camera-client.env
```

정상 출력은 `root root 600 /etc/camera-client.env`이다.

### 9.3 unit 적용 및 부팅 자동 시작

```bash
sudo systemctl daemon-reload
sudo systemctl enable --now camera-client.service
```

`enable`은 부팅 시 자동 시작 링크를 만들고, `--now`는 현재 서비스도 즉시 시작한다.

### 9.4 설치 직후 검증

```bash
systemctl is-enabled camera-client.service
systemctl is-active camera-client.service
systemctl status camera-client.service --no-pager
journalctl -u camera-client.service --since '5 minutes ago' --no-pager
```

첫 두 명령은 각각 `enabled`, `active`를 출력해야 한다. 이어서 서버에서 프레임 증가를
확인한다.

```bash
curl --fail --show-error http://192.168.45.89:3000/api/health
curl --fail --show-error http://192.168.45.89:3000/api/cameras
```

마지막으로 브라우저를 새로고침하고 카메라 선택 목록에서 `CAMERA_ID`를 선택한다.

## 10. 일상적인 서비스 운영

```bash
# 현재 상태
systemctl status camera-client.service --no-pager

# 실시간 로그
journalctl -u camera-client.service --follow

# 최근 100줄
journalctl -u camera-client.service --lines 100 --no-pager

# 시작, 중지, 재시작
sudo systemctl start camera-client.service
sudo systemctl stop camera-client.service
sudo systemctl restart camera-client.service
```

unit의 `Restart=on-failure`, `RestartSec=5` 설정 때문에 비정상 종료 시 5초 후 다시
시작한다. 사용자가 `systemctl stop`으로 중지한 경우에는 자동 재시작하지 않는다.

## 11. 설정만 변경하는 방법

서버 주소, 토큰, 카메라 ID, 해상도, FPS 또는 품질만 바꿀 때는 재빌드하지 않는다.

```bash
sudoedit /etc/camera-client.env
sudo stat --format='%U %G %a %n' /etc/camera-client.env
sudo systemctl restart camera-client.service
systemctl status camera-client.service --no-pager
```

환경 파일 변경에는 `daemon-reload`가 필요하지 않다. `restart`할 때 systemd가 환경
파일을 다시 읽는다. 토큰 변경 시 서버와 Pi의 값을 함께 변경해야 하며, 불일치하면
카메라 인증이 실패한다.

해상도나 FPS를 올린 후에는 서버의 `droppedFrames`, Pi CPU·온도·메모리와 네트워크
사용량을 확인한다.

```bash
curl --fail --show-error http://192.168.45.89:3000/api/cameras
top
vcgencmd measure_temp
```

`backpressure`는 서버가 처리 중인 프레임을 쌓지 않기 위한 정상적인 드롭이다. 지속적으로
영상이 끊기면 `CAMERA_FPS`, 해상도 또는 `JPEG_QUALITY`를 낮춘다.

## 12. 코드 수정 절차

### 12.1 수정 위치

| 경로 | 책임 |
| --- | --- |
| `include/camera_client/config.hpp` | 공개 설정 구조와 파서 계약 |
| `src/config.cpp` | 환경 변수, CLI 우선순위와 범위 검증 |
| `src/camera_process.cpp` | `rpicam-vid`, MJPEG 읽기와 JPEG 분리 |
| `include/camera_client/latest_frame_queue.hpp` | 최신 프레임 한 개만 보관 |
| `src/socket_io_client.cpp` | Engine.IO/Socket.IO 연결, 인증, binary event, ACK |
| `src/application.cpp` | 캡처·전송 조정, 재연결, 통계, 종료 |
| `src/main.cpp` | signal, 설정 파싱과 프로세스 종료 코드 |
| `tests/test_main.cpp` | 하드웨어 독립 회귀 테스트 |
| `packaging/camera-client.service` | systemd 실행·보안·재시작 정책 |
| `Makefile` | 빌드, 테스트, 설치 인터페이스 |

### 12.2 코드 리뷰 규칙

- C++20을 유지한다.
- 공개 함수의 입력, 반환값과 실패 조건을 Doxygen 주석으로 설명한다.
- 시스템·표준·외부 라이브러리 함수의 중요한 반환 의미를 호출 지점에 기록한다.
- 토큰, JPEG 전체 데이터와 민감한 URL query를 로그에 쓰지 않는다.
- 캡처와 네트워크 처리를 한 스레드로 합치지 않는다.
- 큐 크기를 무제한으로 늘리거나 backpressure 프레임을 재전송하지 않는다.
- signal handler에서는 atomic 종료 플래그 이외의 정리 작업을 하지 않는다.
- 자식 프로세스, 파일 디스크립터, WebSocket과 스레드 수명을 RAII 또는 명시적 정상
  종료 흐름으로 관리한다.
- 프로토콜을 변경할 때는 서버 `camera.gateway.ts`, 이 클라이언트와 문서를 같은 변경에서
  갱신한다.

### 12.3 수정 후 필수 검증

```bash
cd /home/pi/workspace/rasberry_pi/client
make ci
git diff --check
```

카메라나 네트워크 코드를 변경했다면 단위 테스트만으로 완료 처리하지 않고 8절의 실제
Pi 연동 시험을 다시 수행한다. 다음 경우에는 반드시 회귀 테스트를 추가한다.

- 설정 값이나 CLI 옵션 추가
- camera ID 또는 값 범위 변경
- latest-frame 큐 동작 변경
- Socket.IO 패킷/ACK 파싱 변경
- 재연결, 종료 또는 오류 분류 변경

## 13. 수정 코드를 Raspberry Pi로 전달하는 방법

### 13.1 권장: Git 기반 배포

개발 Mac에서 변경을 검토하고 커밋·푸시한다.

```bash
cd /Users/donghun2/workspace/rasberry_pi
git status --short
git diff --check
git add client .github/workflows/client-ci.yml README.md
git commit -m 'client: describe the change'
git push origin main
```

Raspberry Pi에서 fast-forward로 가져온다.

```bash
cd /home/pi/workspace/rasberry_pi
git status --short --branch
git pull --ff-only origin main
```

Pi에 커밋하지 않은 변경이 있으면 먼저 원인을 확인한다. `git reset --hard`나 강제 checkout으로
덮어쓰지 않는다.

### 13.2 개발 중: rsync 기반 전달

커밋 전 빠른 하드웨어 시험에는 Mac에서 다음처럼 동기화할 수 있다.

```bash
cd /Users/donghun2/workspace/rasberry_pi
rsync -az \
  --exclude build \
  client/ \
  pi@192.168.45.4:/home/pi/workspace/rasberry_pi/client/
```

이 명령은 원격 파일을 무조건 삭제하지 않는다. `--delete`는 잘못된 경로에서 원격 작업을
지울 수 있으므로 배포 스크립트가 충분히 검증되기 전에는 사용하지 않는다. rsync 배포
후에도 Pi에서 `make ci`를 실행해야 한다.

## 14. 새 코드 배포 방법

빌드 실패가 현재 운영 서비스에 영향을 주지 않도록 **빌드와 테스트를 먼저** 끝낸 뒤
서비스를 중지한다.

```bash
cd /home/pi/workspace/rasberry_pi/client
make ci
```

현재 운영 바이너리를 롤백용으로 보관한다.

```bash
sudo cp --archive \
  /opt/camera-client/bin/camera-client \
  /opt/camera-client/bin/camera-client.previous
```

검증된 새 바이너리와 최신 unit을 설치하고 서비스를 재시작한다.

```bash
sudo systemctl stop camera-client.service
sudo make install
sudo systemctl daemon-reload
sudo systemctl start camera-client.service
```

배포 후 smoke test:

```bash
systemctl is-active camera-client.service
journalctl -u camera-client.service --since '2 minutes ago' --no-pager
curl --fail --show-error http://192.168.45.89:3000/api/health
curl --fail --show-error http://192.168.45.89:3000/api/cameras
```

`receivedFrames`가 증가하고 브라우저가 갱신되어야 배포가 완료된 것이다. 실패하면 다음
절의 롤백을 즉시 수행한다.

## 15. 롤백

이전 바이너리가 남아 있다면 다음 순서로 복구한다.

```bash
sudo systemctl stop camera-client.service
sudo install --mode=0755 \
  /opt/camera-client/bin/camera-client.previous \
  /opt/camera-client/bin/camera-client
sudo systemctl start camera-client.service
systemctl status camera-client.service --no-pager
```

unit도 변경했다면 이전 Git commit의 `packaging/camera-client.service`를 복원한 뒤
`sudo make install`, `sudo systemctl daemon-reload`를 수행한다. 환경 설정 변경 전에는
별도로 root 전용 백업을 만들 수 있다.

```bash
sudo cp --archive /etc/camera-client.env /etc/camera-client.env.previous
```

설정을 롤백한 후에는 `sudo systemctl restart camera-client.service`가 필요하다. 백업에도
토큰이 들어 있으므로 권한을 `0600`으로 유지하고 필요 없어진 백업은 안전하게 제거한다.

## 16. CI/CD 연결

저장소의 `.github/workflows/client-ci.yml`은 `client/**` 또는 workflow 변경 시 Ubuntu
runner에서 다음과 같은 작업을 수행한다.

1. 소스 checkout
2. C++/Boost/JSON 빌드 의존성 설치
3. `make -C client ci`

일반 GitHub runner에는 Raspberry Pi 카메라가 없으므로 컴파일과 단위 테스트까지만
담당한다. 실제 배포 pipeline은 다음 단계로 분리하는 것을 권장한다.

```text
pull request
  └─ x86 Linux build + unit tests
main merge
  └─ arm64 Raspberry Pi build + unit tests
       └─ 승인된 배포
            ├─ 기존 바이너리 백업
            ├─ systemd stop/install/start
            └─ health/cameras smoke test
```

배포 job은 GitHub secret의 토큰을 명령행 인자로 출력하지 않아야 한다. 가능하면 토큰은
Pi의 `/etc/camera-client.env`에만 유지하고 CI는 바이너리와 unit만 교체한다. SSH 개인 키,
카메라 토큰, 환경 파일은 artifact나 빌드 로그에 포함하지 않는다.

## 17. 장애 해결

### 17.1 웹 화면은 열리지만 실시간 영상이 없음

```bash
systemctl is-active camera-client.service
pgrep -af 'camera-client|rpicam-vid'
curl --fail --show-error http://192.168.45.89:3000/api/cameras
```

- 서비스가 `inactive`이면 시작한다.
- `connected: false`이면 클라이언트가 서버에 인증·접속하지 못했거나 중지된 상태다.
- `connected: true`인데 `receivedFrames`가 증가하지 않으면 카메라/인코딩 로그를 확인한다.
- 브라우저에서 올바른 `CAMERA_ID`를 선택했는지 확인한다.

### 17.2 서비스가 시작되지 않음

```bash
systemctl status camera-client.service --no-pager
journalctl -u camera-client.service --boot --no-pager
sudo systemd-analyze verify /etc/systemd/system/camera-client.service
sudo test -r /etc/camera-client.env
sudo test -x /opt/camera-client/bin/camera-client
```

환경 파일이 없거나 필수 값이 비어 있으면 설정 오류와 함께 종료되고 systemd가 재시작을
반복한다.

### 17.3 서버 연결 실패

```bash
curl --fail --show-error http://192.168.45.89:3000/api/health
ip route
```

- Pi에서 실제 서버 LAN IP를 사용한다.
- 서버 Docker의 3000 포트가 호스트에 공개됐는지 확인한다.
- Mac IP가 DHCP로 변경됐다면 `/etc/camera-client.env`를 수정하고 서비스를 재시작한다.
- 현재 클라이언트는 `https://`를 지원하지 않으므로 URL scheme을 확인한다.

### 17.4 인증 실패

- Pi의 `CAMERA_TOKEN`이 서버 프로세스의 `CAMERA_TOKEN`과 정확히 같은지 확인한다.
- 토큰 값을 로그나 화면에 출력하지 않는다.
- 환경 파일 수정 후 서비스를 재시작했는지 확인한다.
- `CAMERA_ID`가 1~64자의 영문자, 숫자, `_`, `-`로만 구성됐는지 확인한다.

### 17.5 카메라 열기 실패 또는 사용 중

```bash
pgrep -af 'rpicam|libcamera|camera-client'
rpicam-hello --list-cameras
id pi
```

다른 프로세스가 같은 카메라를 사용 중이면 하나만 남긴다. systemd 서비스 실행 중에
수동 `camera-client`를 동시에 실행하지 않는다.

### 17.6 높은 드롭률 또는 끊김

- `CAMERA_FPS`를 10에서 8 또는 5로 낮춘다.
- 해상도를 1280×720 이하로 낮춘다.
- `JPEG_QUALITY`를 80에서 70 정도로 낮춘다.
- 서버 vision 처리 시간과 CPU 사용량을 확인한다.
- `backpressure` 자체는 오류가 아니며 최신 영상 유지를 위한 정상 동작이다.

### 17.7 로그에 토큰이 노출됨

즉시 서버와 Pi의 토큰을 새 값으로 교체하고 서비스를 재시작한다. 노출된 CI 로그나
artifact의 접근 범위를 차단하고 보존 정책에 따라 제거한다. Git에 커밋됐다면 단순히
최신 파일에서 지우는 것만으로는 과거 이력이 사라지지 않으므로 토큰 폐기가 우선이다.

## 18. 서비스 제거

서비스를 더 이상 사용하지 않을 때 먼저 중지하고 부팅 자동 시작을 해제한다.

```bash
sudo systemctl disable --now camera-client.service
cd /home/pi/workspace/rasberry_pi/client
sudo make uninstall
sudo systemctl daemon-reload
sudo systemctl reset-failed
```

환경 파일에는 인증 토큰이 있으므로 백업 필요성을 확인한 뒤 별도로 제거한다.

```bash
sudo rm /etc/camera-client.env
```

`make uninstall`은 운영 바이너리와 systemd unit만 제거하며 소스, 빌드 디렉터리와 환경
파일은 자동으로 삭제하지 않는다.

## 19. 최종 운영 체크리스트

- [ ] Pi에서 서버 `/api/health` 접근 성공
- [ ] `rpicam-hello --list-cameras`에서 카메라 확인
- [ ] `pi` 사용자가 `video` 그룹에 포함됨
- [ ] 빌드 의존성 설치 완료
- [ ] `make ci` 경고 없이 통과
- [ ] 수동 실행에서 Socket.IO 인증 성공
- [ ] 서버 `/api/cameras`의 `receivedFrames` 증가 확인
- [ ] `/etc/camera-client.env` 소유자 `root:root`, 권한 `0600`
- [ ] `systemd-analyze verify` 통과
- [ ] 서비스가 `enabled` 및 `active`
- [ ] 재부팅 후 서비스와 실시간 영상 자동 복구 확인
- [ ] 토큰이 Git, 로그, 명령행과 artifact에 없음
- [ ] 코드 변경 전 운영 바이너리 또는 배포 commit 확인
- [ ] 배포 후 로그, health, cameras와 브라우저 smoke test 통과
- [ ] 실패 시 사용할 롤백 바이너리와 절차 확인
