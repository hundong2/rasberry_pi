# Raspberry Pi Real-time Camera Viewer

Raspberry Pi 카메라의 JPEG 프레임을 C++20 클라이언트로 전송하고, NestJS와
OpenCV 서버에서 처리한 영상을 웹 브라우저에 실시간으로 표시하는 프로젝트입니다.

현재는 움직임 감지, 에지 영상, 원본 통과 모드를 지원합니다. 이후 사람·사물 및
휴대폰 화면의 버튼 인식 모델을 연결할 수 있도록 카메라 송신, 스트림 중계, 영상
처리를 분리했습니다.

## 주요 기능

- Raspberry Pi 카메라 프레임 수집 및 JPEG 인코딩
- C++20 클라이언트의 Socket.IO/WebSocket 실시간 전송
- 토큰 기반 카메라 연결 인증과 프레임 크기·FPS 제한
- OpenCV 기반 움직임 감지, 에지 처리 및 원본 통과
- 브라우저에서 카메라 선택, 영상, FPS, 처리 시간, 검출 결과 확인
- 처리 지연 시 오래된 프레임을 버리는 latest-frame/backpressure 제어
- Make 기반 C++ 빌드·테스트와 systemd 상시 실행
- GitHub Actions에서 C++ 클라이언트 자동 빌드·테스트

## 전체 구조

```text
Raspberry Pi Camera
        │
        │ JPEG binary / Socket.IO namespace: /stream
        ▼
C++20 Camera Client
        │ camera:frame
        ▼
NestJS Server ───── internal HTTP ─────► Python FastAPI + OpenCV
        ▲                                      │
        └──── processed frame + metadata ──────┘
        │ viewer:frame
        ▼
Web Browser
```

카메라는 `camera:frame` 이벤트로 JPEG binary와 촬영 시각을 전송합니다. 서버는
OpenCV 처리 결과를 `viewer:frame` 이벤트로 브라우저에 전달합니다. 처리 중 새
프레임이 들어오면 지연이 계속 쌓이지 않도록 오래된 프레임을 재전송하지 않고
드롭할 수 있습니다.

## 저장소 구조

```text
.
├── client/                  # Raspberry Pi용 C++20 카메라 송신 클라이언트
│   ├── include/             # 공개 헤더와 자료형
│   ├── src/                 # 카메라, 큐, Socket.IO, 실행 흐름 구현
│   ├── tests/               # 하드웨어 비의존 단위 테스트
│   ├── packaging/           # systemd unit
│   └── Makefile             # 빌드·테스트·설치 타깃
├── server/                  # NestJS 서버, 웹 UI, OpenCV 서비스
│   ├── src/                 # HTTP와 Socket.IO 서버
│   ├── public/              # 브라우저 뷰어
│   ├── vision/              # FastAPI + OpenCV 영상 처리
│   └── raspberry-pi/        # 기존 Python 송신기 예제
├── docs/                    # 연결 원리, 코드 학습, 비전 학습 문서
├── .github/workflows/       # CI 워크플로
└── TODO.md                  # 성능 측정 및 최적화 작업 목록
```

## 요구 사항

서버는 Docker Compose로 실행하는 방법을 권장합니다. 직접 실행하려면 Node.js 22
이상과 Python 3.11 이상이 필요합니다.

C++ 클라이언트는 Raspberry Pi OS Bookworm, C++20 컴파일러, GNU Make,
Boost.System, libcamera의 `rpicam-vid`를 기준으로 합니다. 실제 패키지
설치 명령은 [`client/install.md`](client/install.md)에 정리되어 있습니다.

## 빠른 시작

### 1. 서버 실행

```bash
cd server
cp .env.example .env
```

`server/.env`의 `CAMERA_TOKEN`을 예측하기 어려운 값으로 변경한 다음 실행합니다.

```bash
docker compose up --build
```

기본 접속 주소는 다음과 같습니다.

- 웹 뷰어: `http://SERVER_IP:3000`
- 상태 확인: `http://SERVER_IP:3000/api/health`
- 연결된 카메라 목록: `http://SERVER_IP:3000/api/cameras`

`VIEWER_TOKEN`이 비어 있으면 웹 뷰어가 공개됩니다. LAN 외부에 서비스할 때는
viewer 인증과 HTTPS reverse proxy를 함께 구성해야 합니다.

### 2. Raspberry Pi 클라이언트 빌드

```bash
cd client
make
make test
```

빌드 결과는 `client/build/camera-client`에 생성되며 Git에 포함되지 않습니다.

### 3. 클라이언트 수동 실행

서버의 `CAMERA_TOKEN`과 같은 값을 셸에 노출하지 않고 입력합니다.

```bash
cd client
read -rsp 'CAMERA_TOKEN: ' CAMERA_TOKEN
echo
export CAMERA_TOKEN
export CAMERA_SERVER_URL='http://SERVER_IP:3000'
export CAMERA_ID='raspberry-pi-1'
./build/camera-client
```

`read -rsp`에서 입력하는 값은 새로 만드는 별도 토큰이 아니라 서버
`server/.env`에 설정한 `CAMERA_TOKEN`입니다. 입력 문자는 화면에 표시되지 않으며
Enter를 누르면 실행 환경 변수로 전달할 수 있습니다.

클라이언트의 전체 설정 예제는
[`client/config/camera-client.env.example`](client/config/camera-client.env.example),
systemd 등록과 배포 절차는 [`client/install.md`](client/install.md)를 참고하세요.

## 주요 설정

| 구분 | 변수 | 기본값 | 설명 |
|---|---|---:|---|
| Server | `PORT` | `3000` | API와 웹 뷰어 포트 |
| Server | `CAMERA_TOKEN` | 없음 | 카메라 연결 인증값, 운영 환경 필수 |
| Server | `VIEWER_TOKEN` | 빈 값 | 웹 뷰어 인증값, 빈 값이면 공개 |
| Server | `OPENCV_URL` | `http://localhost:8000` | OpenCV 서비스 내부 주소 |
| Server | `MAX_CAMERA_FPS` | `12` | 카메라별 서버 처리 FPS 상한 |
| Server | `VISION_MODE` | `motion` | `motion`, `edges`, `none` |
| Client | `CAMERA_SERVER_URL` | 예제 주소 | NestJS 서버 주소 |
| Client | `CAMERA_TOKEN` | 없음 | 서버와 동일한 카메라 토큰 |
| Client | `CAMERA_ID` | `raspberry-pi-1` | 장치별 고유 ID |
| Client | `CAMERA_WIDTH` / `CAMERA_HEIGHT` | `1280` / `720` | 전송 해상도 |
| Client | `CAMERA_FPS` | `10` | 카메라 촬영 FPS |
| Client | `JPEG_QUALITY` | `80` | JPEG 품질 |

토큰과 `.env`는 커밋하지 마세요. 저장소는 실제 환경 파일과 빌드 산출물,
의존성, 런타임 소켓을 `.gitignore`로 제외합니다.

## 개발 및 검증

서버 변경 사항은 다음 명령으로 검증합니다.

```bash
cd server
npm install
npm run typecheck
npm test
npm run build
```

클라이언트는 Raspberry Pi 또는 필요한 C++ 라이브러리가 설치된 Linux 환경에서
전체 CI 절차를 실행합니다.

```bash
cd client
make ci
```

`make ci`는 기존 빌드 디렉터리를 정리한 뒤 C++20 클라이언트와 테스트를 새로
빌드하고 단위 테스트를 실행합니다.

## 문서 안내

- [`server/README.md`](server/README.md): 서버 구조, Docker 실행, 프로토콜과 운영 설정
- [`client/README.md`](client/README.md): C++20 클라이언트 설계, 코드 리뷰 기준과 빌드
- [`client/install.md`](client/install.md): 의존성 설치, systemd 등록, 수정·배포·롤백
- [`docs/getting_start.md`](docs/getting_start.md): TypeScript와 C++ 코드를 읽는 권장 순서 및 필수 이론
- [`docs/remote-ssh-connect.md`](docs/remote-ssh-connect.md): `codex-pi.sock`과 SSH 연결 재사용 원리
- [`docs/vision-object-ui-training-guide.md`](docs/vision-object-ui-training-guide.md): 사람·사물·휴대폰 UI 인식 데이터와 모델 학습 가이드
- [`TODO.md`](TODO.md): 속도 측정, 병목 확인, 최적화 우선순위

## 현재 범위와 다음 단계

현재 구현은 단일 Raspberry Pi 카메라의 LAN 실시간 모니터링을 위한 MVP입니다.
다수 카메라, 인터넷 구간, 고해상도·고FPS 환경에서는 Socket.IO room, Redis
adapter, TLS, H.264/WebRTC, 이벤트 녹화 정책을 추가로 검토해야 합니다.

사람·사물·UI 버튼 인식은 아직 기본 기능에 포함되지 않습니다. 먼저 실제 사용
환경의 데이터와 성능 기준을 정의한 다음 모델을 학습하고 OpenCV 서비스에
연결하는 순서가 권장됩니다.

## 라이선스

이 프로젝트는 [MIT License](LICENSE)를 따릅니다.
