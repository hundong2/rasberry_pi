# Camera real-time viewer server

Raspberry Pi가 촬영한 JPEG 프레임을 WebSocket으로 받고, OpenCV로 분석한 결과를 브라우저에 실시간으로 전달하는 서버입니다.

## 기술 스택

- **NestJS 12 + TypeScript 6 + Socket.IO**: 모듈화, 의존성 주입, WebSocket 지원이 성숙한 실무형 Node.js 구성입니다.
- **Python FastAPI + OpenCV**: Node 네이티브 바인딩의 빌드/ABI 문제를 피하고, 향후 YOLO·ONNX·VLM 모델을 쉽게 연동할 수 있습니다.
- **Docker Compose**: API와 영상 처리 서비스를 독립적으로 배포하고 확장하기 쉽습니다.
- **JPEG over WebSocket**: 첫 버전의 구현과 장애 분석이 단순합니다. 20 FPS 이상, 다수 카메라, 외부망 서비스에는 WebRTC/H.264가 더 적합합니다.

```text
Raspberry Pi (Picamera2)
       │ JPEG / Socket.IO
       ▼
NestJS ingestion API ── internal HTTP ──► FastAPI + OpenCV
       │                                      │
       └──── processed JPEG + metadata ◄──────┘
       │ Socket.IO
       ▼
Web browser viewer
```

## 개발 계획과 현재 상태

1. **기반 구성 — 완료**: NestJS 프로젝트, 환경 변수, health API, Docker 구성
2. **실시간 수신 — 완료(MVP)**: 카메라 인증, JPEG 검증, FPS 제한, 처리 중 프레임 드롭
3. **OpenCV 처리 — 완료(MVP)**: 움직임 검출/바운딩 박스, 엣지 보기, 원본 통과 모드
4. **웹 뷰어 — 완료(MVP)**: 카메라 선택, 처리 시간·검출 수·수신 FPS 표시
5. **Raspberry Pi 연동 — 완료(예제)**: Picamera2 송신기와 systemd 자동 실행 예제
6. **다음 이터레이션 — 예정**: 객체 인식, Redis 수평 확장, 이벤트 녹화, TLS, 부하 테스트

## 빠른 실행: Docker Compose

```bash
cd server
cp .env.example .env
# .env의 CAMERA_TOKEN을 충분히 긴 임의 문자열로 변경
docker compose up --build
```

- 웹 뷰어: `http://SERVER_IP:3000`
- 상태 확인: `http://SERVER_IP:3000/api/health`
- 카메라 목록: `http://SERVER_IP:3000/api/cameras`

`VIEWER_TOKEN`이 비어 있으면 뷰어는 공개됩니다. LAN 밖에 노출할 때는 값을 설정하고 HTTPS reverse proxy를 사용하세요.

## 로컬 개발

Node.js 22 이상과 Python 3.11 이상이 필요합니다.

터미널 1 — OpenCV 서비스:

```bash
cd server/vision
python3 -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt
uvicorn app:app --reload --port 8000
```

터미널 2 — NestJS 서버:

```bash
cd server
cp .env.example .env
npm install
npm run start:dev
```

검증:

```bash
npm run typecheck
npm test
npm run build
```

## Raspberry Pi 송신 설정

Raspberry Pi OS Bookworm과 Camera Module 기준입니다. 먼저 `rpicam-hello --timeout 5000`으로 카메라를 확인합니다.

C++ 송신기를 AI 코딩 에이전트와 개발할 때는 [`raspberry-pi/AI_CPP_DEVELOPMENT_GUIDE.md`](raspberry-pi/AI_CPP_DEVELOPMENT_GUIDE.md)의 프로토콜, 구조, 보안, 테스트 지침을 기준으로 사용하세요.

```bash
sudo apt update
sudo apt install -y python3-picamera2 python3-opencv python3-venv
mkdir -p ~/camera-viewer
cd ~/camera-viewer
python3 -m venv --system-site-packages .venv
source .venv/bin/activate
pip install -r requirements.txt
```

`raspberry-pi/sender.py`와 `requirements.txt`를 위 디렉터리에 복사하고 실행합니다. `picamera2`와 OpenCV는 Raspberry Pi OS 패키지를 사용하므로 `--system-site-packages`가 중요합니다.

```bash
python sender.py \
  --server http://192.168.0.10:3000 \
  --token '서버의-CAMERA_TOKEN' \
  --camera-id raspberry-pi-1 \
  --width 1280 --height 720 --fps 10 --quality 80
```

### 부팅 시 자동 실행

`raspberry-pi/camera-viewer.service` 예제를 `/etc/systemd/system/`에 복사하고, 다음 환경 파일을 만듭니다.

```bash
sudo tee /etc/camera-viewer.env >/dev/null <<'ENV'
SERVER_URL=http://192.168.0.10:3000
CAMERA_TOKEN=change-this-camera-token
CAMERA_ID=raspberry-pi-1
ENV
sudo chmod 600 /etc/camera-viewer.env
sudo systemctl daemon-reload
sudo systemctl enable --now camera-viewer
journalctl -u camera-viewer -f
```

서비스 예제는 코드를 `/opt/camera-viewer`에 설치했다고 가정합니다. `User=pi`와 경로를 실제 환경에 맞게 변경하세요.

## 프로토콜

Socket.IO namespace는 `/stream`, transport는 WebSocket입니다.

카메라 연결 인증:

```json
{ "role": "camera", "cameraId": "raspberry-pi-1", "token": "..." }
```

카메라는 `camera:frame` 이벤트로 `{ timestamp, frame }`을 보냅니다. `timestamp`는 Unix epoch milliseconds, `frame`은 Base64가 아닌 JPEG binary입니다. 서버는 `viewer:frame`으로 `cameraId`, `timestamp`, `processingMs`, `detections`, `frame`을 전달합니다. `backpressure` 응답은 오류가 아니라 지연 누적 방지를 위한 드롭이며 재전송하지 않습니다.

## 설정값

| 변수 | 기본값 | 설명 |
|---|---:|---|
| `PORT` | `3000` | API/웹 서버 포트 |
| `CAMERA_TOKEN` | 없음 | 카메라 인증 토큰, 운영 환경 필수 |
| `VIEWER_TOKEN` | 빈 값 | 비어 있으면 누구나 시청 가능 |
| `OPENCV_URL` | `http://localhost:8000` | 내부 OpenCV 서비스 주소 |
| `MAX_FRAME_BYTES` | `2097152` | 프레임 최대 크기 |
| `MAX_CAMERA_FPS` | `12` | 카메라별 서버 처리 FPS 상한 |
| `VISION_MODE` | `motion` | `motion`, `edges`, `none` |
| `JPEG_QUALITY` | `80` | 처리 영상 JPEG 품질(30~95로 제한) |

## 운영 전 체크리스트

- 카메라별 토큰 또는 mTLS로 인증을 강화하고, Caddy/Nginx에서 TLS를 적용합니다.
- 여러 API 인스턴스에는 Socket.IO Redis adapter와 카메라별 고정 라우팅이 필요합니다.
- 카메라 수가 늘면 모든 viewer 브로드캐스트 대신 Socket.IO room 구독을 구현합니다.
- 인터넷 구간과 고해상도 스트림에는 JPEG 대신 WebRTC/H.264를 사용합니다.
- 영상은 기본적으로 저장하지 않습니다. 녹화 기능에는 보관 기한, 접근 통제, 개인정보 정책이 필요합니다.

## 문제 해결

- 영상이 없음: `/api/health`의 `vision`, Pi의 `journalctl`, `docker compose logs -f`를 확인합니다.
- `processing_failed`: OpenCV 연결 또는 JPEG 디코딩 실패입니다.
- `backpressure`가 많음: Pi FPS/해상도/quality를 낮추거나 vision worker를 확장합니다.
