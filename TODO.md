# Camera Streaming / Vision TODO

## 목적

현재 Raspberry Pi 카메라 스트리밍의 측정 결과를 기준으로 성능 병목과 객체·사람·모바일
UI 인식 확장 작업을 우선순위별로 관리한다. 체크박스는 구현뿐 아니라 테스트와 문서화까지
완료됐을 때만 `[x]`로 변경한다.

### 이 TODO가 필요한 이유

영상 시스템은 “화면이 나온다”만으로 품질을 판단하기 어렵다. FPS를 높이면 지연, CPU,
발열, 대역폭이 함께 변하고 AI 모델을 추가하면 정확도와 처리량이 서로 충돌한다. 작업의
이유와 완료 증거가 없으면 다음 사람이 같은 실험을 반복하거나, 단순히 FPS 숫자만 좋아진
변경을 최적화라고 오해할 수 있다.

각 체크박스는 다음 세 질문에 답해야 완료된다.

1. **왜 하는가?** 해결하려는 사용자 증상 또는 기술적 위험이 무엇인가?
2. **안 하면 어떻게 되는가?** 지연, drop, 장애, 보안 또는 정확도에 어떤 문제가 남는가?
3. **무엇으로 증명하는가?** 전후 측정, 테스트, 로그와 문서 중 어떤 evidence가 있는가?

## 2026-09-01 기준 측정 결과

측정 조건:

- Raspberry Pi 5, IMX708 Wide
- 1280×720, MJPEG quality 80, 10 FPS
- C++20 `camera-client` → NestJS → FastAPI/OpenCV motion → 브라우저
- 서버: Mac Docker, `VISION_MODE=motion`

### 10초 구간 측정

| 항목 | 첫 구간 | 두 번째 구간 | 해석 |
| --- | ---: | ---: | --- |
| 클라이언트 입력 | 10.0 FPS | 10.0 FPS | 목표 입력률 유지 |
| 서버 처리/뷰어 전달 가능량 | 약 8.6 FPS | 약 8.0 FPS | 현재 실제 표시 상한 |
| 서버 드롭 | 14/100 (14%) | 20/100 (20%) | 처리 중 또는 FPS gate 드롭 |
| Pi eth0 총 송신 | 미측정 | 약 0.74 MB/s | SSH 등 다른 트래픽 포함 |
| Pi eth0 총 수신 | 미측정 | 약 0.61 MB/s | 카메라 전용 수치가 아님 |

누적 API 표본은 `receivedFrames=3472`, `droppedFrames=580`으로 약 16.7%가
드롭된 상태였다. `receivedFrames`는 처리 전에 증가하므로 처리 완료 프레임은
`receivedFrames - droppedFrames`로 해석한다.

### 리소스 표본

| 구성 요소 | CPU | RSS/메모리 | 상태 |
| --- | ---: | ---: | --- |
| Pi `camera-client` | 0.7% | 약 4.5 MiB | 여유 |
| Pi `rpicam-vid` | 7.6% | 약 114 MiB | 여유 |
| 서버 NestJS container | 3.21% | 약 82.9 MiB | 여유 |
| 서버 vision container | 17.14% | 약 50.6 MiB | 현재 주요 연산 경로 |

- Pi 온도: 48.3°C
- Pi throttling: `0x0`(없음)
- Pi load average: `0.07 / 0.08 / 0.06`
- Pi 가용 메모리: 약 7.2 GiB

### 현재 병목 판단

Pi 캡처와 C++ 전송기는 충분히 여유가 있다. 현재 경로는 모든 입력 프레임에 대해 다음
복사·변환을 직렬로 수행한다.

```text
JPEG binary 수신
  → Node FormData/multipart 생성
  → Python에서 JPEG decode
  → full-resolution motion 처리
  → JPEG re-encode
  → Base64 encode + JSON 응답
  → Node에서 Base64 decode
  → Socket.IO binary broadcast
  → 브라우저 Blob/Object URL 생성
```

최적화의 첫 대상은 카메라나 네트워크가 아니라 서버 vision 처리 빈도와 Base64 왕복이다.

## P0 — 측정 가능성과 안정성

P0인 이유: 측정 도구 없이 최적화부터 하면 어느 변경이 실제로 좋아졌는지 알 수 없다.
평균값만 보면 순간적인 멈춤, 메모리 누수와 재연결 실패도 놓치므로 기능 확장 전에 반드시
기준선을 만든다.

| 작업 | 왜 필요한가 | 미수행 위험 | 완료 증거 |
| --- | --- | --- | --- |
| 이동 구간 metrics | 순간 FPS/drop과 장기 추세를 분리 | “가끔 끊김” 원인을 재현하지 못함 | 1분/5분 지표와 dashboard/API |
| C++ 구조화 통계 | Pi 캡처·큐·전송 중 어느 단계가 느린지 구분 | 서버 문제를 Pi 문제로 잘못 판단 | capture/sent/ACK/size 로그 |
| end-to-end latency | FPS가 높아도 오래된 화면일 수 있음 | 수 초 늦은 영상을 실시간으로 오판 | p50/p95 latency 측정 |
| soak test | 누수·발열·재연결 문제는 짧은 시험에 안 보임 | 운영 중 수십 분 후 중단 | 30분 자원 그래프와 복구 로그 |
| 수치 기준 | 사람마다 “빠르다” 판단이 다름 | 회귀를 merge 후에 발견 | PR/배포 gate와 전후 표 |

- [ ] 서버에 1분/5분 이동 구간 지표를 추가한다.
  - camera별 input FPS, accepted FPS, dropped FPS
  - drop reason(`fps_gate`, `processing_busy`, `invalid`, `vision_error`)
  - vision `processingMs` p50/p95/p99
  - JPEG input/output byte p50/p95
  - viewer broadcast FPS와 연결 viewer 수
- [ ] C++ 클라이언트 통계를 구조화한다.
  - capture/sent/accepted/backpressure/reconnect/queue-replaced
  - 평균·최대 JPEG 크기
  - 10초 주기 한 줄 로그 또는 로컬 metrics endpoint
- [ ] end-to-end latency를 측정한다.
  - 캡처 `timestamp`와 브라우저 수신 시각 차이
  - Pi와 서버의 시각 동기화 상태도 함께 기록
- [ ] 30분 soak test를 수행한다.
  - Pi/서버 RSS 증가 여부
  - 카메라 재시작, 서버 재시작, 네트워크 20초 단절 후 자동 복구
  - 최대 온도와 throttling
- [ ] 테스트 기준을 수치로 고정한다.
  - 1280×720 기준 viewer FPS ≥ 9 또는 설정한 목표의 90%
  - p95 end-to-end latency ≤ 500 ms
  - 30분 RSS 증가 ≤ 10%
  - 정상 네트워크에서 reconnect 0회

## P0 — 즉시 적용 가능한 스트리밍 최적화

P0인 이유: 현재 10 FPS 입력 중 14~20%가 버려지고 있으며, Pi가 아니라 서버의 반복
decode/encode/Base64 경로가 병목으로 확인됐다. 모델을 추가하기 전에 이 경로를 줄여야 AI
추론에 사용할 CPU와 latency budget을 확보할 수 있다.

| 작업 | 왜 필요한가 | 미수행 위험 | 완료 증거 |
| --- | --- | --- | --- |
| 분석/표시 FPS 분리 | 사람은 부드러운 화면, AI는 낮은 추론 빈도로도 충분 | 추론 1회 지연이 화면 전체 정지로 전파 | display 9~10 FPS + inference 3~5 FPS |
| Base64 제거 | Base64는 크기 증가와 encode/decode·복사를 추가 | camera 수 증가 시 CPU/GC/대역폭 낭비 | binary 전후 CPU·bytes·p95 비교 |
| `none` fast path | 분석 없는 mode에는 decode/re-encode 이유가 없음 | 원본 전달만 해도 불필요한 화질 손실/지연 | 원본 byte 전달 test와 latency |
| 축소 motion | motion 위치는 저해상도로도 찾을 수 있음 | full HD pixel을 매 frame 계산 | 동일 recall에서 processing p95 감소 |
| 목표 FPS 정렬 | 처리 능력보다 많이 보내면 drop/ACK만 증가 | 대역폭 소비와 불안정한 viewer FPS | drop < 목표값, viewer FPS 안정 |

- [ ] **분석 FPS와 표시 FPS를 분리한다.**
  - 객체/모션 분석은 3~5 FPS로 실행
  - 원본 프레임은 8~10 FPS로 viewer에 전달
  - 최신 detection을 timestamp와 함께 캐시해 중간 프레임에 재사용
  - AI 추론 시간이 길어져도 실시간 화면이 멈추지 않게 한다.
- [ ] FastAPI 응답의 Base64 JSON 왕복을 제거한다.
  - 후보 A: JPEG binary body + metadata header
  - 후보 B: `multipart/mixed`로 metadata JSON과 JPEG binary 반환
  - 후보 C: vision 결과는 metadata만 반환하고 원본 JPEG는 Node가 그대로 broadcast
  - 구현 후 output byte, CPU와 processing p95를 전후 비교한다.
- [ ] `VISION_MODE=none`에서는 decode/re-encode 없이 원본 JPEG를 그대로 전달한다.
- [ ] 모션 검출은 축소 grayscale 이미지에서 수행한다.
  - 예: 1280×720 → 640×360 또는 320×180
  - 검출 좌표만 원본 해상도로 scale-up
  - 원본 영상에는 scale-up한 box를 그리거나 브라우저 overlay로 그린다.
- [ ] 설정 목표를 처리 능력과 일치시킨다.
  - 구조 변경 전 임시값: client 8 FPS 또는 `MAX_CAMERA_FPS=8`
  - 입력 10 FPS를 유지하려면 처리 경로 개선 후 다시 측정
  - 단순 FPS 제한만으로 성능 개선 완료로 판단하지 않는다.

## P1 — 서버 파이프라인 개선

P1인 이유: 단일 camera에서는 현재 구조가 동작하지만 요청 callback이 vision 완료까지 기다리고
namespace 전체에 broadcast한다. camera/viewer가 늘면 느린 한 단계가 연결 수만큼 복제된다.

| 작업 | 왜 필요한가 | 미수행 위험 | 완료 증거 |
| --- | --- | --- | --- |
| latest-frame worker | ingress와 느린 vision 수명을 분리 | 처리 대기 frame이 쌓이거나 ACK가 지연 | queue capacity 1 test, drop reason |
| Socket.IO room | viewer가 선택한 camera만 받아야 함 | N camera × M viewer traffic 폭증 | room별 수신 integration test |
| metadata overlay | box를 그리기 위한 JPEG 재인코딩 제거 | AI 기능마다 화질 저하와 CPU 증가 | browser overlay와 원본 hash/품질 비교 |
| browser 계측 | server가 빨라도 rendering이 느릴 수 있음 | backend만 최적화하고 UI 끊김 유지 | decode/render p95와 dropped render 수 |
| worker 수 검증 | 병렬화가 항상 빨라지는 것은 아님 | CPU 경쟁·순서 역전·메모리 폭증 | 1/2/N worker benchmark |
| timeout/circuit breaker | vision 장애를 정상 stream과 격리 | 장애 난 vision에 요청이 계속 몰림 | failure injection 및 자동 복구 test |

- [ ] camera별 단일 최신-frame worker를 명시적으로 구현한다.
  - gateway callback에서 긴 vision HTTP 요청을 직접 기다리지 않는다.
  - queue capacity 1, 새 프레임이 오래된 대기 프레임을 교체
  - 처리 중 프레임 수와 drop reason을 관측 가능하게 한다.
- [ ] viewer 구독을 Socket.IO room으로 변경한다.
  - 현재 모든 `viewer:frame`을 namespace 전체에 broadcast
  - viewer가 선택한 camera room만 구독
  - camera/viewer 수가 늘어날 때 불필요한 네트워크·브라우저 작업 방지
- [ ] bounding box를 JPEG에 매번 그리지 않고 metadata overlay로 분리하는 방안을 평가한다.
  - 서버 JPEG 재인코딩 제거 가능
  - 브라우저 canvas/SVG overlay로 label, confidence와 box 표시
- [ ] 브라우저 렌더러를 계측·개선한다.
  - `Uint8Array.byteOffset/byteLength`를 보존해 정확한 Blob 범위 사용
  - Object URL 생성/폐기 비용과 `img.decode()` 대기 시간 측정
  - 필요하면 `createImageBitmap` + canvas latest-frame 렌더링 비교
- [ ] vision service worker 수를 무작정 늘리기 전에 camera별 순서와 메모리 사용을 검증한다.
- [ ] 프레임 처리 timeout, 재시도 금지와 circuit breaker 정책을 문서화한다.

## P1 — 객체·사람 탐지 MVP

P1인 이유: `person`과 `cell phone`은 pretrained detector가 이미 잘 아는 class일 가능성이
높다. 바로 custom 학습하면 데이터 수집 비용과 라이선스 부담을 만들 수 있으므로 실제
카메라 baseline으로 부족함을 먼저 증명해야 한다.

| 작업 | 왜 필요한가 | 미수행 위험 | 완료 증거 |
| --- | --- | --- | --- |
| pretrained baseline | custom 학습 필요 여부 판단 | 이미 해결된 문제에 학습 비용 지출 | 실제 IMX708 test precision/recall |
| `objects` mode | motion과 객체 의미를 설정/운영에서 분리 | mode별 장애·성능 비교 불가능 | API schema 및 mode test |
| Mac server 우선 | 모델 교체·관측·rollback이 Pi보다 쉬움 | edge 최적화와 서버 통합을 동시에 디버깅 | server CPU baseline |
| 낮은 추론 FPS/cache | detection은 display마다 새로 계산할 필요가 없음 | AI 추가 즉시 viewer FPS 급락 | inference/display 분리 지표 |
| runtime/INT8/HAT 비교 | hardware별 빠른 runtime이 다름 | 이름만 보고 accelerator를 구매 | 같은 model/test set benchmark |
| license 검토 | code와 weight도 배포 조건이 있음 | MIT 프로젝트의 배포 조건 충돌 | license manifest와 승인 기록 |

- [ ] pretrained 경량 detector로 `person`, `cell phone` baseline을 먼저 측정한다.
  - 새 학습 전에 실제 IMX708 영상의 precision/recall과 FPS 확인
  - confidence/NMS threshold를 config로 노출
- [ ] `VISION_MODE=objects`를 추가한다.
  - 기존 `motion`, `edges`, `none` 계약 유지
  - `detections[].type`에 실제 class name 사용
- [ ] AI 추론은 우선 Mac vision container에서 실행한다.
  - 현재 Pi보다 서버가 모델 교체·관측·재배포하기 쉬움
  - Pi inference는 네트워크 독립성 또는 대역폭 절감이 필요할 때 별도 비교
- [ ] 객체 추론 빈도를 3~5 FPS로 제한하고 tracker/cached detection을 적용한다.
- [ ] CPU baseline, ONNX Runtime, INT8, 선택적 Raspberry Pi AI HAT을 같은 데이터로 benchmark한다.
- [ ] 모델·학습 코드·weight의 라이선스를 배포 전에 검토한다.
  - 특히 Ultralytics 코드/모델은 AGPL-3.0 또는 별도 Enterprise 조건 확인

## P1 — 스마트폰 화면과 버튼 인식 MVP

P1인 이유: camera 속 스마트폰은 screenshot과 달리 기울고 반사되며 UI 요소가 매우 작다.
일반 object detector 하나에 모든 역할을 맡기면 “화면을 못 찾은 것인지, 버튼을 못 찾은
것인지, 글자를 못 읽은 것인지” 실패 원인을 구분할 수 없다.

| 작업 | 왜 필요한가 | 미수행 위험 | 완료 증거 |
| --- | --- | --- | --- |
| 2단계 문제 정의 | 화면 geometry와 UI semantics를 분리 | 거대한 단일 모델의 오류 분석 불가 | stage별 metric과 API |
| homography | 기울어진 button 좌표를 정면 좌표로 통일 | 앱/각도마다 box 모양 변화 | corner error와 round-trip test |
| detector + OCR | 새 문구를 class 추가 없이 읽음 | 버튼 문자열 수만큼 class 폭증 | button hit rate + CER |
| 작은 icon class | 충분한 class별 data 확보 가능 | long-tail class 불균형·강제 오분류 | per-class sample/unknown 정책 |
| 실제 IMX708 data | screenshot에는 glare/moiré/blur가 없음 | lab에서는 높고 현장에서는 낮은 정확도 | 실제-camera holdout 결과 |
| group split | 연속 frame은 거의 동일함 | test leakage로 과장된 mAP | device/app/session split manifest |
| 개인정보 처리 | 화면에는 메시지·계정 정보가 있음 | 학습 archive/로그를 통한 정보 노출 | consent·mask·retention 기록 |
| click hit/OCR 평가 | mAP가 높아도 실제 버튼 선택은 틀릴 수 있음 | 자동화가 옆 버튼을 선택 | click-point hit rate와 CER |

- [ ] 요구사항을 두 단계로 고정한다.
  1. 카메라 영상에서 `phone_screen` 영역 탐지/segmentation
  2. 정면 보정된 화면에서 `button`, `icon`, `text_field`, `toggle` 등 UI element 탐지
- [ ] 스마트폰 네 모서리 또는 screen mask를 이용한 homography 보정을 구현한다.
- [ ] 텍스트 버튼은 `button detector + OCR` 조합으로 처리한다.
- [ ] 아이콘 의미는 초기에 작은 class 집합으로 제한한다.
  - 예: `back`, `home`, `menu`, `search`, `send`, `close`
  - 모든 앱의 모든 아이콘을 한 번에 분류하지 않는다.
- [ ] 실제 IMX708 촬영 데이터셋을 만든다.
  - 거리, 각도, 반사, 모아레, 밝기, dark/light theme, 기기/앱 다양성
  - 화면 캡처 데이터만으로 학습 완료로 판단하지 않는다.
- [ ] device/app/session 단위로 train/val/test를 분리해 연속 프레임 누수를 막는다.
- [ ] 개인 메시지, 계정, 알림과 식별 정보를 수집 전에 마스킹하거나 제외한다.
- [ ] UI 인식 평가에 bbox mAP 외 click-point hit rate와 OCR 정확도를 추가한다.

## P2 — 대역폭과 확장성

P2인 이유: 현재 단일 camera/LAN에서는 JPEG가 단순하고 디버깅하기 쉽다. 하지만 camera와
viewer가 늘거나 외부망으로 이동하면 frame마다 완전한 JPEG를 보내는 비용이 선형으로
커진다. P0/P1 기준선 없이 codec/분산 구조부터 바꾸면 복잡도만 늘 수 있어 P2로 둔다.

**완료 원칙:** 동일 장면·동일 목표 화질에서 latency, Mbps, CPU, 복구 시간을 비교하고
“새 기술을 붙였다”가 아니라 기존 목표 대비 개선된 수치로 판단한다.

- [ ] 단일 카메라·LAN을 넘기기 전에 JPEG와 H.264/WebRTC를 비교한다.
  - 동일 화질의 평균 Mbps
  - encode/decode CPU
  - end-to-end latency
  - 패킷 손실과 재연결 동작
- [ ] 다수 카메라에는 Redis adapter와 camera affinity/sticky routing을 설계한다.
- [ ] 이벤트 녹화는 motion/object event 전후 ring buffer로 제한한다.
- [ ] 모델 서버를 별도 process/container로 유지할지 Pi edge inference로 이동할지 비용을 측정한다.
- [ ] AI HAT 사용 시 Hailo 모델 변환, 지원 op, 정확도와 실제 FPS를 별도 기록한다.

## P2 — 품질·보안·개인정보

P2로 분류했지만 외부망 공개, 다중 사용자, 실제 개인 화면 저장을 시작하는 순간에는 해당
배포의 P0 차단 조건으로 승격한다. 기능 prototype 단계와 실제 운영의 위험 수준이 다르기
때문이다.

| 작업 | 왜 필요한가 | 미수행 위험 | 완료 증거 |
| --- | --- | --- | --- |
| HTTPS/WSS | token과 영상 도청 방지 | LAN 밖 평문 노출 | 인증서 검증 integration test |
| 장치별 인증 | 한 token 유출 범위 제한 | 한 장치 유출로 전체 camera 위조 | rotate/revoke test |
| 얼굴 인식 분리 | 존재 탐지와 신원 식별 위험이 다름 | 동의 없는 biometric 처리 | 범위·법적 검토 문서 |
| 보존/접근 정책 | 스마트폰 화면은 민감 정보 가능 | 무기한 저장·과도한 접근 | retention job과 access audit |
| license manifest | data/model마다 조건이 다름 | 배포·재배포 권리 위반 | artifact별 license 기록 |
| 오류 data 검토 | active learning이 민감 frame을 모을 수 있음 | 자동 cloud 업로드로 정보 유출 | 사람 승인·mask workflow |

- [ ] 외부망 사용 전에 HTTPS/WSS와 서버 인증서 검증을 구현한다.
- [ ] 장치별 camera token 또는 mTLS로 확장한다.
- [ ] 사람 탐지는 신원 식별과 분리한다. 얼굴 인식은 별도 동의·법적 검토 없이 추가하지 않는다.
- [ ] 스마트폰 화면 원본의 저장 여부, 보존 기간, 접근 권한과 삭제 정책을 정의한다.
- [ ] 학습 데이터와 모델 artifact에 dataset/model license manifest를 포함한다.
- [ ] 모델 실패 사례를 수집할 때 민감 화면을 자동 업로드하지 않고 사용자 검토 단계를 둔다.

## 다음 실행 순서

1. P0 metrics와 end-to-end latency 측정 추가
2. 분석 FPS/표시 FPS 분리
3. Base64 왕복 제거
4. pretrained `person`/`cell phone` baseline
5. 스마트폰 screen rectification prototype
6. UI element custom dataset 1차 수집·학습
7. ONNX/INT8/AI HAT benchmark 후 배포 위치 결정

상세 학습 절차는 [`docs/vision-object-ui-training-guide.md`](docs/vision-object-ui-training-guide.md)를
참고한다.
