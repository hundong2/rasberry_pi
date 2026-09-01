# 객체·사람·스마트폰 UI 인식 학습 가이드

## 1. 목적과 범위

현재 시스템의 모션 검출을 다음 수준으로 확장하기 위한 데이터 수집, 학습, 평가, 모델
내보내기와 서버 통합 절차를 설명한다.

1. 일반 객체와 사람 탐지
2. 카메라 영상 속 휴대폰과 화면 영역 탐지
3. 휴대폰 화면 안의 버튼·아이콘·입력창 등 UI 요소 탐지
4. 버튼 텍스트 또는 아이콘 의미 인식

이 문서는 학습 로드맵이다. 특정 모델을 무조건 채택하거나 현재 코드에 학습 의존성을
즉시 추가하지 않는다. 모델 정확도, 속도, 라이선스와 개인정보 조건을 검증한 뒤 실제
구현 작업을 별도로 진행한다.

### 왜 학습부터 시작하지 않는가

AI 기능이 필요하다고 바로 데이터를 모으고 GPU 학습을 시작하면, pretrained 모델로 이미
해결되는지, 실제 문제는 화면 원근인지 OCR인지, 운영 장비가 목표 FPS를 감당하는지 알 수
없다. 학습은 데이터 label, GPU 비용, model version과 라이선스를 장기간 유지해야 하는
선택이다. 따라서 이 가이드는 **문제 정의 → baseline → 데이터 → 학습 → 평가 → 최적화 →
통합** 순서를 따른다.

| 순서를 건너뛸 때 | 흔한 결과 |
| --- | --- |
| 문제 정의 없이 학습 | class가 계속 바뀌어 label을 다시 작성 |
| baseline 없이 custom 학습 | 이미 pretrained로 되던 기능에 시간 소비 |
| 실제-camera test 없이 screenshot 학습 | 학습 점수는 높지만 glare/모아레에서 실패 |
| 정확도 평가 없이 quantization | 빨라졌지만 특정 class가 사라짐 |
| 시스템 측정 없이 모델 통합 | inference가 화면 FPS까지 멈춤 |
| license 확인 없이 weight 배포 | 저장소 license와 model 조건 충돌 |

## 2. 현재 시스템과 확장 지점

### 왜 현재 구조부터 이해해야 하는가

AI model은 혼자 실행되지 않고 capture, network, decode, inference, encode, browser 중 한
단계가 된다. 현재 어느 process가 frame 소유권을 가지고 어떤 형식으로 넘기는지 모르면
model latency와 HTTP/Base64 비용을 구분할 수 없다. 특히 지금은 Pi가 아니라 FastAPI 처리
경로가 병목이므로 model을 Pi에 옮기는 것이 자동으로 정답이 아니다.

현재 영상 경로:

```text
Pi camera-client
  → JPEG / Socket.IO
  → NestJS CameraGateway
  → FastAPI server/vision/app.py
  → OpenCV motion/edges/none
  → processed JPEG + detections
  → browser viewer
```

`server/vision/app.py`의 `/process`와 `VISION_MODE`가 주 확장 지점이다. 서버의
`Detection` 타입은 이미 `type`, `confidence`, `x`, `y`, `width`, `height`를 가지므로
사람·사물·UI element box를 같은 응답 구조로 전달할 수 있다.

권장 mode:

| Mode | 용도 | 권장 추론 빈도 |
| --- | --- | ---: |
| `none` | 원본 실시간 표시 | 8~10 FPS |
| `motion` | 현재 움직임 영역 | 3~10 FPS |
| `objects` | person, cell phone, 일반 객체 | 3~5 FPS |
| `phone_ui` | 화면 보정 + UI element + OCR | 1~3 FPS 또는 요청 시 |

AI 처리가 느려도 화면 표시가 멈추지 않도록 표시 FPS와 추론 FPS를 분리하고 최신 detection을
짧게 캐시한다.

## 3. 먼저 결정해야 할 문제 정의

### 왜 문제를 세분화해야 하는가

`사람이 있는가`, `휴대폰이 어디 있는가`, `화면의 네 모서리가 어디인가`, `어떤 버튼이며
무슨 글자인가`는 label과 평가 기준이 서로 다른 문제다. 하나의 “인식 정확도” 숫자로
합치면 어느 단계가 실패했는지 알 수 없고 필요한 data도 정할 수 없다.

### 3.1 사람·사물 탐지

탐지는 “영상 어디에 어떤 class가 있는가”를 bounding box로 반환한다.

최초 class 후보:

- `person`
- `cell_phone`
- 프로젝트에 실제 필요한 소수 객체

사람 탐지는 신원 식별이 아니다. 사람의 존재/위치 탐지와 얼굴 인식은 데이터·법적 위험이
다르므로 별도 요구와 명시적 동의 없이 얼굴 인식을 범위에 넣지 않는다.

### 3.2 스마트폰 버튼 인식의 의미

“휴대폰 버튼”은 세 가지로 나눠야 한다.

| 문제 | 예 | 권장 방식 |
| --- | --- | --- |
| 물리 버튼 | 전원, 볼륨 | 휴대폰 측면/버튼 custom object detection |
| 일반 UI element | 버튼, 입력창, 토글 | 화면 rectification 후 UI element detector |
| 의미 있는 버튼 | 뒤로, 검색, 전송 | UI detector + icon classifier 또는 OCR |

이 가이드의 중심은 카메라에 보이는 **휴대폰 화면 내부 UI**이다.

앱을 직접 제어할 수 있고 Android view hierarchy, Accessibility API 또는 UIAutomator에
접근할 수 있다면 픽셀 인식보다 구조화된 UI 정보를 우선한다. 외부 카메라만 사용할 수
있거나 실제 기기 화면 상태를 시각적으로 검증해야 할 때 vision 접근이 필요하다.

## 4. 권장 전체 파이프라인

### 왜 단계형 pipeline인가

휴대폰 화면은 평면이므로 먼저 기하학적으로 정면 보정하면 이후 UI detector와 OCR이 각도
변화를 학습해야 하는 부담이 줄어든다. detector, OCR, icon semantics를 분리하면 새 버튼
문구는 OCR만, 새 icon은 classifier만 갱신할 수 있어 재학습 범위와 rollback 위험이 작다.

```text
원본 카메라 프레임
       │
       ├─ 1) person / cell_phone detector
       │
       └─ 2) phone_screen detector 또는 segmentation
                    │
                    ▼
          screen corner/mask → homography → 정면 화면
                    │
                    ├─ 3) UI element detector
                    │      button, icon, text_field, toggle ...
                    │
                    ├─ 4) OCR
                    │      버튼/필드의 표시 문자열
                    │
                    └─ 5) icon semantics classifier/grounder
                           back, search, send, close ...
```

한 모델이 전체를 한 번에 해결하도록 시작하지 않는다. 화면 기하 보정과 UI 의미 인식을
분리하면 데이터 오류와 실패 원인을 단계별로 분석할 수 있다.

## 5. 사람·일반 객체: 학습 전 baseline

### 왜 pretrained baseline이 먼저인가

사람과 휴대폰은 널리 사용되는 object-detection dataset의 대표 class다. 실제 IMX708
장면에서도 pretrained 모델이 요구 precision/recall을 만족한다면 custom 학습은 정확도
이득 없이 dataset 유지 비용만 만든다. 반대로 실패 장면을 baseline에서 모으면 custom
학습에 정말 필요한 hard example을 알 수 있다.

COCO 계열 pretrained detector는 보통 `person`, `cell phone` class를 포함한다. 새로
학습하기 전에 실제 IMX708 영상으로 baseline을 만든다.

### 5.1 baseline 데이터

실제 운영 환경에서 10~30분 영상을 수집하고 다음 조건을 포함한다.

- 사람이 가까이/멀리 있는 장면
- 일부만 보이는 사람
- 세로/가로 방향 휴대폰
- 휴대폰 화면 켜짐/꺼짐
- 역광, 저조도, 실내 조명
- 사람이 없거나 휴대폰이 없는 negative 장면
- 사진·모니터 속 사람/휴대폰처럼 혼동하기 쉬운 장면

연속 영상의 거의 같은 프레임을 모두 평가 데이터로 사용하지 않는다. 1~2 FPS sampling과
perceptual hash 중복 제거로 장면 다양성을 높인다.

### 5.2 baseline 평가

최소 200~500장의 대표 frame에 ground-truth box를 붙인다. pretrained 모델을 그대로
실행해 다음을 기록한다.

- class별 precision, recall, mAP50, mAP50-95
- confidence threshold별 false positive/false negative
- 1280×720 입력의 preprocessing + inference + NMS p50/p95
- 서버 CPU/RSS와 viewer FPS

baseline이 요구 정확도를 만족하면 custom training을 하지 않는다. threshold 조정과 실제
파이프라인 최적화가 더 빠르고 데이터 유지 비용도 없다.

## 6. 스마트폰 화면 인식: 두 단계 데이터셋

### 왜 화면과 UI 요소를 분리하는가

카메라 영상에서 button은 작고 기울어져 있지만 screenshot 속 button은 정면이다. 화면
위치를 먼저 찾고 정면으로 펴면 UI model의 입력 분포가 단순해진다. 이 단계를 생략하면
UI detector가 button 모양뿐 아니라 모든 기기 각도와 원근까지 동시에 학습해야 하므로 더
많은 data가 필요하고 좌표 정확도도 낮아진다.

### 6.1 1단계 — 화면 위치와 모서리

단순 `cell_phone` bounding box는 화면 내부 버튼 좌표를 안정적으로 계산하기 어렵다.
화면 자체를 다음 중 하나로 annotation한다.

- `phone_screen` segmentation mask — 가장 권장
- 화면의 네 모서리 keypoint — 원근 보정에 직접 사용
- screen bounding box — 정면에 가까운 고정 카메라 MVP

segmentation mask의 윤곽을 사각형으로 근사하거나 네 corner로부터 homography를 계산해
정면 화면으로 변환한다. OpenCV 공식 문서의 `findHomography`와 `warpPerspective`가 이
단계의 핵심이다.

### 6.2 2단계 — UI element

정면 보정된 화면에서 다음처럼 시각적으로 구분 가능하고 실제 기능에 필요한 class만
선택한다.

```text
button
icon
text
text_field
checkbox
radio
toggle
tab
navigation_item
dialog
```

초기에는 `button`, `icon`, `text_field`, `toggle` 정도로 시작한다. `로그인`, `확인`,
`삭제`처럼 버튼 문구를 각각 detector class로 만들지 않는다. 일반 `button` box를 찾은
뒤 OCR로 문구를 읽는 편이 새 문자열과 다국어에 확장하기 쉽다.

아이콘은 텍스트가 없으므로 다음과 같이 작은 의미 class 집합을 별도로 학습할 수 있다.

```text
back, home, menu, search, send, close, add, delete
```

## 7. 데이터 수집 설계

### 왜 데이터 설계가 모델 선택보다 중요한가

모델은 학습 data에 없는 조명, 기기, 언어와 가림을 스스로 추측할 수 없다. 같은 영상을
수천 장 복사한 dataset보다 서로 다른 기기·세션 500장이 운영 일반화에 더 유용할 수 있다.
수집 축과 개인정보 규칙을 학습 전에 정해야 나중에 test leakage나 data 폐기로 전체
annotation을 다시 하는 일을 피할 수 있다.

### 7.1 실제 카메라 데이터가 필수인 이유

스마트폰 screenshot 데이터는 깨끗한 디지털 이미지다. IMX708로 실제 화면을 촬영하면
다음 domain shift가 생긴다.

- 원근 왜곡과 렌즈 왜곡
- 화면 반사와 주변 물체 반영
- moiré, rolling shutter, PWM/flicker
- autofocus 실패와 motion blur
- 노출 차이, 색온도, glare
- 화면 bezel, notch, rounded corner
- 손가락이나 손에 의한 가림

RICO 같은 screenshot 데이터로 pretraining할 수 있지만, 최종 validation/test는 반드시
실제 카메라 촬영으로 구성한다.

### 7.2 촬영 축

| 축 | 포함할 값 예시 |
| --- | --- |
| 기기 | 서로 다른 화면 크기·해상도·bezel |
| 방향 | portrait, landscape |
| 거리 | 근거리, 중거리, 작은 화면 비율 |
| 각도 | 정면, 좌우/상하 15°·30°·45° |
| 밝기 | dark room, 일반, 강한 조명, 역광 |
| theme | light, dark, high contrast |
| 언어 | 한국어, 영어, 실제 지원 언어 |
| UI 상태 | enabled, disabled, pressed, loading, dialog |
| 가림 | 손가락, 케이스, 부분 화면 |
| negative | TV/모니터, 종이, 빈 책상, 휴대폰 뒷면 |

### 7.3 권장 초기 규모

다음은 시작점이지 정확도 보장은 아니다.

| 단계 | 초기 목표 |
| --- | ---: |
| pretrained person/phone baseline | label 200~500장 |
| phone screen/mask custom | 다양한 실제 촬영 500~1,500장 |
| UI element custom | screenshot+실촬영 2,000장 이상 |
| icon semantics | class별 최소 200개, long-tail은 추가 수집 |

데이터 수보다 독립적인 기기·앱·세션·조명 조건과 정확한 label 품질이 더 중요하다.

### 7.4 개인정보

- 개인 메시지, 알림, 이메일, 전화번호, 계정, 사진과 위치가 보이는 화면은 수집하지 않는다.
- 필요한 경우 촬영 직후 자동/수동 masking 후 annotation한다.
- 원본 영상 저장 기간과 접근자를 정한다.
- 타인의 얼굴/화면을 동의 없이 학습 데이터로 사용하지 않는다.
- cloud annotation/training에 업로드할 때 조직의 데이터 정책을 확인한다.

## 8. Annotation 규칙

### 왜 annotation guideline이 필요한가

두 사람이 같은 화면을 보고 한 명은 icon만, 다른 사람은 icon을 포함한 button 전체를
그리면 모델에는 서로 모순된 정답이 들어간다. 모델 loss는 label의 의도를 알지 못하므로
annotation 불일치를 더 많은 epoch로 해결할 수 없다. box 범위, 가림, tiny object와 nested
element 규칙을 문서화해야 평가 점수도 재현 가능하다.

### 8.1 도구

CVAT처럼 object detection, segmentation, keypoint와 YOLO export를 지원하는 도구를 사용할
수 있다. 도구보다 class 정의와 일관된 annotation guideline이 중요하다.

### 8.2 공통 규칙

- 화면 밖으로 잘린 객체도 보이는 부분이 충분하면 annotation한다.
- 완전히 가려졌거나 사람이 구별할 수 없는 객체는 억지로 label하지 않는다.
- UI element box는 그림자보다 실제 interactive area 기준으로 일관되게 정의한다.
- nested UI는 목적에 맞게 정의한다. 예: icon이 button 안에 있을 때 둘 다 필요한지 결정한다.
- tiny object 최소 크기와 `ignore` 정책을 문서화한다.
- annotator 간 불일치 샘플을 정기적으로 리뷰한다.

### 8.3 Dataset split

연속 frame을 무작위로 나누면 거의 같은 이미지가 train과 test에 들어가 성능이 부풀려진다.
다음 단위로 그룹 분할한다.

- 기기 모델
- 앱
- 촬영 세션/날짜
- 장소와 조명 세팅

권장 시작 비율은 train 70%, validation 15%, test 15%다. 최종 test는 학습과 threshold
조정에 사용하지 않는다. 운영 카메라와 다른 기기/앱을 test에 일부 남겨 일반화를 확인한다.

## 9. Screenshot 데이터 활용

### 왜 공개 screenshot과 실제 촬영을 함께 쓰는가

공개 UI dataset은 class와 layout 다양성을 저렴하게 제공하지만 카메라 artifact가 없다.
실제 촬영만 사용하면 반대로 다양한 앱/icon을 충분히 모으기 어렵다. 공개 data는
pretraining/coverage, 합성 변형은 보강, 실제 IMX708 holdout은 최종 현실성 검증 역할로
분리한다.

### 9.1 RICO / RICO Semantics

RICO는 수만 개 모바일 UI screenshot과 view hierarchy를 제공한다. RICO Semantics는
icon shape/semantics, UI element와 text label 연결, human-annotated box를 추가한다.

활용 방법:

1. UI element detector 또는 icon semantics의 pretraining
2. screenshot과 view hierarchy에서 자동 초기 box 생성
3. 실제 프로젝트 class taxonomy로 mapping
4. 실제 카메라 domain으로 fine-tuning

주의 사항:

- RICO 원 데이터의 hierarchy/semantic label에는 noise가 있을 수 있다.
- RICO Semantics 저장소는 2026년 현재 archive/read-only 상태다.
- RICO Semantics는 공개 저장소에 CC BY-SA 4.0으로 표시되어 있으므로 데이터·파생물의
  실제 배포 조건을 검토한다.
- 공개 데이터에 없는 최신 앱 UI와 한국어 화면은 자체 수집이 필요하다.

### 9.2 합성 카메라 augmentation

깨끗한 screenshot을 실제 camera-like image로 변환한다.

- random perspective/homography
- blur와 defocus
- brightness/gamma/color temperature
- glare/gradient overlay
- moiré 또는 downsample/upsample
- bezel/background 합성
- finger/hand occlusion
- JPEG compression

합성 데이터만으로 test하지 않는다. 합성은 실제 촬영을 대체하는 것이 아니라 class와
각도 다양성을 늘리는 보조 수단이다.

## 10. 학습 환경

### 왜 학습과 inference 장비를 분리하는가

학습은 수많은 image를 반복 계산해 GPU와 큰 memory가 필요하지만 inference는 한 frame을
정해진 latency 안에 처리해야 한다. Pi에서 학습하면 시간이 오래 걸리고 실행 중 camera
service와 자원을 경쟁한다. GPU에서 재현 가능한 학습 artifact를 만들고, 실제 Pi/Mac에서는
export model의 속도와 정확도만 검증하는 편이 장애 원인을 분리하기 쉽다.

학습은 Raspberry Pi가 아니라 CUDA GPU가 있는 workstation/cloud에서 수행하는 것을
권장한다. Pi와 현재 Mac 서버는 inference benchmark와 통합 검증에 사용한다.

재현성을 위해 다음을 기록한다.

- Python, framework, CUDA/cuDNN 버전
- package lock 또는 정확한 package version
- dataset version/checksum
- train/val/test manifest
- random seed
- model 이름과 pretrained weight checksum
- 입력 크기, batch, epoch, augmentation, optimizer
- Git commit과 실행 명령

토큰/자격 증명은 `.env` 또는 secret store로 관리하고 notebook이나 dataset archive에
넣지 않는다.

## 11. Ultralytics YOLO를 이용한 빠른 실험 예시

### 왜 “빠른 실험”으로 한정하는가

YOLO 도구는 dataset 검증부터 train/export까지 빠르게 baseline을 만들 수 있지만, 편리함이
운영 채택을 자동으로 정당화하지 않는다. 목표 정확도, runtime 호환성, weight와 code의
license를 검토한 뒤 유지할지 결정한다. 명령 예제의 최신 모델명을 그대로 복사하기보다
검증한 package/model version을 고정해야 같은 결과를 다시 만들 수 있다.

Ultralytics의 현재 공식 문서는 custom dataset training과 ONNX 등 다양한 export를
지원한다. 빠른 baseline에는 편리하지만 **라이선스를 먼저 확인해야 한다**. 공식 안내는
AGPL-3.0 공개 조건 또는 별도 Enterprise license가 적용될 수 있다고 설명한다. 현재
저장소가 MIT라고 해서 model code/weight의 의무가 자동으로 MIT로 바뀌지 않는다.

아래는 구조 예시이며 실제 사용 시 framework 버전을 pin한다.

```text
datasets/phone_ui/
├── images/
│   ├── train/
│   ├── val/
│   └── test/
├── labels/
│   ├── train/
│   ├── val/
│   └── test/
└── data.yaml
```

Detection용 `data.yaml` 예시:

```yaml
path: /absolute/path/to/datasets/phone_ui
train: images/train
val: images/val
test: images/test
names:
  0: phone_screen
  1: button
  2: icon
  3: text_field
  4: toggle
```

학습 흐름 예시:

```bash
python3 -m venv .venv-train
source .venv-train/bin/activate
python -m pip install --upgrade pip
python -m pip install 'ultralytics==검증한-고정-버전'

yolo detect train \
  model=경량-pretrained-model.pt \
  data=/absolute/path/to/datasets/phone_ui/data.yaml \
  imgsz=640 \
  epochs=100 \
  batch=-1 \
  project=runs/phone_ui \
  name=baseline_v1
```

`batch=-1` 같은 옵션의 현재 의미와 지원 여부는 설치한 고정 버전의 공식 문서로 다시
확인한다. segmentation 또는 keypoint가 필요하면 detect task가 아니라 해당 task 형식과
annotation을 사용한다.

검증:

```bash
yolo detect val \
  model=runs/phone_ui/baseline_v1/weights/best.pt \
  data=/absolute/path/to/datasets/phone_ui/data.yaml \
  split=test
```

ONNX export 예시:

```bash
yolo export \
  model=runs/phone_ui/baseline_v1/weights/best.pt \
  format=onnx \
  imgsz=640 \
  simplify=True
```

export 후 framework `.pt` 결과와 ONNX 결과를 같은 test set에서 비교한다. export 성공만
확인하고 정확도 검증 없이 배포하지 않는다.

## 12. 평가 지표

### 왜 mAP 하나로 판단하면 안 되는가

mAP는 dataset 전체 detection 품질을 비교하기 좋지만 사용자가 누를 좌표, OCR 문자열,
지연과 특정 기기의 실패를 모두 설명하지 않는다. 예를 들어 큰 button은 잘 찾고 작은
`삭제` icon만 놓쳐도 전체 mAP는 높을 수 있다. 그래서 model metric, task metric,
system metric을 동시에 release gate로 사용한다.

### 12.1 객체/화면/UI element

- precision, recall
- mAP50, mAP50-95
- class별 confusion matrix
- 작은 객체와 부분 가림 subset
- 기기/앱/조명별 slice 성능
- false positive per minute

### 12.2 실제 버튼 선택 품질

자동화 관점에서는 mAP만으로 부족하다.

- predicted center가 target element 안에 들어가는 비율(click-point hit rate)
- IoU threshold별 hit rate
- 화면 전체 좌표로 역변환했을 때의 pixel 오차
- 같은 모양의 여러 버튼 중 semantic target을 고르는 정확도
- disabled/hidden element 오탐률

### 12.3 OCR

- character error rate(CER)
- word error rate(WER)
- 한국어/영어/숫자/특수문자별 slice
- 작은 글자, glare, blur 조건별 정확도

### 12.4 시스템

- preprocessing/inference/NMS p50/p95
- 전체 vision processing p50/p95
- viewer FPS와 end-to-end latency
- CPU, RSS, 온도, 평균 네트워크 Mbps
- 30분 이상 메모리 안정성

## 13. Model 최적화와 배포 형식

### 왜 정확도 검증 뒤에 최적화하는가

입력 크기 축소와 INT8 quantization은 속도를 높일 수 있지만 작은 UI element의 정보와
weight 표현력을 줄인다. 최적화 전 FP32 기준선이 없으면 정확도 손실이 export 문제인지
원래 model 문제인지 구분할 수 없다. 항상 같은 test set으로 FP32와 최적화본을 비교한다.

### 13.1 ONNX Runtime

ONNX Runtime은 framework에서 export한 모델을 여러 하드웨어 execution provider에서
실행할 수 있고 graph optimization을 적용한다. 현재 Mac vision container에서 CPU
baseline을 만들기 좋다.

권장 순서:

1. FP32 ONNX 정확도와 latency 측정
2. 입력 크기 640/512/416/320 비교
3. static INT8 quantization을 representative calibration set으로 시험
4. FP32 대비 class별 정확도 손실 측정
5. 실제 Docker image 크기와 cold start 측정

ONNX Runtime 공식 문서는 CNN에는 일반적으로 static quantization을 먼저 고려하되,
quantization이 항상 빨라지는 것은 아니며 accuracy를 다시 검증해야 한다고 설명한다.

### 13.2 Raspberry Pi CPU

Pi CPU에서 inference할 경우:

- 경량 모델과 작은 입력 크기부터 시작
- inference를 1~5 FPS로 제한
- camera capture/encode와 CPU contention 측정
- ARM64에서 ONNX Runtime/LiteRT/NCNN 후보를 동일 model·dataset으로 비교
- Pi에서 모델 학습은 하지 않음

### 13.3 Raspberry Pi AI HAT

Raspberry Pi 공식 문서에 따르면 AI HAT 계열은 지원되는 image recognition/object
detection 작업을 Hailo NPU에서 실행할 수 있다. 구매 또는 채택 전에 다음을 검증한다.

- 사용 모델의 Hailo 변환 가능 여부와 unsupported op
- model compile/calibration 절차
- 실제 input resolution의 FPS와 power/temperature
- custom post-processing/NMS 지원
- runtime와 model artifact license

AI HAT은 현재 CPU 경로의 병목을 자동으로 해결하지 않는다. 현재 측정상 Pi가 아니라 Mac
vision 경로가 병목이므로, edge inference로 옮길 목적(네트워크 절감, 독립 동작, privacy)을
먼저 정의한다.

## 14. 현재 FastAPI vision 서비스 통합 설계

### 왜 model code를 기존 service 경계에 넣는가

현재 NestJS는 인증·camera 상태·viewer broadcast를, FastAPI는 image 처리를 담당한다.
초기 object detector를 FastAPI 경계에 넣으면 Socket.IO 계약을 바꾸지 않고 mode별로
rollback할 수 있다. 단, model 결과와 원본 frame 전달을 분리해 느린 inference가 viewer
표시를 막지 않도록 구조를 개선해야 한다.

### 14.1 모델 로드

- container startup 또는 FastAPI lifespan에서 모델을 한 번 로드한다.
- `/process` 요청마다 model을 다시 생성하지 않는다.
- model path, confidence, NMS IoU, inference size와 inference FPS를 환경 변수로 둔다.
- health endpoint에 model loaded/version/backend를 추가한다.

예상 환경 변수:

```dotenv
VISION_MODE=objects
VISION_MODEL_PATH=/models/object-detector.onnx
VISION_CONFIDENCE=0.35
VISION_NMS_IOU=0.50
VISION_INPUT_SIZE=640
VISION_INFERENCE_FPS=4
```

### 14.2 응답

현재 detection 구조를 유지한다.

```json
{
  "type": "person",
  "confidence": 0.91,
  "x": 120,
  "y": 80,
  "width": 240,
  "height": 500
}
```

phone UI mode에는 필요하면 다음 metadata를 별도 schema version과 함께 추가한다.

- rectified screen 크기
- screen corner 좌표
- OCR text
- element role/icon semantic
- 원본 좌표와 rectified 좌표

기존 viewer와 호환되지 않는 필드는 optional로 추가하고 API schema test를 만든다.

### 14.3 처리 빈도

- 프레임 표시와 추론을 분리한다.
- model inference 중 들어온 오래된 inference frame은 버린다.
- 마지막 detection은 짧은 TTL과 원본 timestamp를 함께 전달한다.
- 사람이 움직이는 장면에는 tracker를 추가할 수 있지만 tracker ID를 신원으로 해석하지 않는다.

### 14.4 JPEG와 overlay

성능을 위해 우선 검토할 구조:

```text
원본 JPEG ───────────────────────────────► browser image
    └─ decode → inference → detection JSON ─► browser canvas/SVG overlay
```

이 구조는 detection box를 그리기 위한 서버 JPEG 재인코딩과 Base64 왕복을 제거할 수 있다.

## 15. 스마트폰 화면 원근 보정

### 왜 homography가 필요한가

화면은 3차원 공간에서 기울어져 보여도 실제 UI는 평면 좌표계에 배치된다. homography는
두 평면 사이 좌표를 변환하므로 기울어진 camera pixel을 정면 screen pixel로 통일할 수
있다. 이 변환이 없으면 같은 button도 각도마다 크기·모양·간격이 달라져 detection과 click
좌표 모두 불안정해진다.

### 15.1 corner/mask에서 정면 화면 생성

1. `phone_screen` mask 또는 네 corner 탐지
2. corner 순서를 좌상/우상/우하/좌하로 정규화
3. target width/height 계산
4. homography 계산
5. `warpPerspective`로 rectified image 생성
6. UI detection/OCR 실행
7. 결과 좌표를 inverse homography로 원본 frame에 mapping

corner 순서 오류는 화면 뒤집힘과 잘못된 button 위치를 만든다. 기하 단계만 별도의 unit
test와 synthetic rectangle test로 검증한다.

### 15.2 고정 환경의 간단한 대안

휴대폰 위치가 고정되고 카메라가 정면이라면 최초 MVP는 수동 ROI 또는 ArUco marker로
screen corner를 보정할 수 있다. 이 방식은 학습 데이터가 적고 빠르지만 자유로운 위치와
일반 사용자 환경에는 맞지 않는다.

## 16. OCR와 icon semantics

### 왜 detector 하나로 버튼 의미를 끝내지 않는가

버튼 문구는 사용자/언어/상태에 따라 계속 바뀌므로 문자열마다 detector class를 만드는
것은 확장되지 않는다. detector는 영역, OCR은 표시 문자열, icon classifier는 텍스트 없는
시각 의미를 담당하게 나누면 새 언어와 icon을 독립적으로 개선할 수 있다.

### 16.1 OCR

UI detector가 찾은 `button`/`text` ROI만 OCR하면 전체 frame OCR보다 빠르고 noise가 적다.

처리 순서:

1. screen rectification
2. UI element ROI crop
3. 필요 시 2× upscale, contrast/denoise
4. 한국어+영어 OCR
5. whitespace/Unicode 정규화
6. detector box와 OCR text 연결

Tesseract, PaddleOCR 또는 다른 runtime을 동일 실제-camera test set으로 비교한다. 모델
다운로드 크기, 한국어 정확도, CPU latency와 라이선스를 함께 기록한다.

### 16.2 icon semantics

RICO Semantics는 icon shape와 semantic annotation의 시작점이 될 수 있다. 다만 운영 앱의
icon 스타일과 다른 경우 실제 앱 데이터로 fine-tuning한다.

초기에는 closed-set classification을 사용하고 낮은 confidence는 `unknown`으로 반환한다.
모든 icon을 억지로 known class로 분류하면 위험한 자동 동작으로 이어질 수 있다.

## 17. Active learning 반복

### 왜 운영 실패 사례를 다시 학습해야 하는가

초기 dataset은 실제 운영의 모든 반사, 앱 update와 사용자 동작을 포함할 수 없다. 그러나
모든 frame을 저장하면 개인정보와 annotation 비용이 폭증한다. low-confidence, disagreement,
사용자 신고처럼 정보량 높은 후보만 사람 검토 후 추가하면 적은 data로 실제 약점을
개선할 수 있다.

```text
초기 model
  → 실제 스트림 shadow inference
  → low-confidence / false-positive 후보 저장
  → 개인정보 검토·마스킹
  → 사람 annotation
  → dataset version 증가
  → retrain / test / compare
  → 승인된 model만 배포
```

무작위로 모든 프레임을 저장하지 않는다. 다음 조건을 중심으로 후보를 수집한다.

- low confidence
- detector 간 disagreement
- 새로운 기기/앱/조명
- 사용자가 명시적으로 오류를 표시한 frame
- negative false positive

## 18. Model registry와 재현성

### 왜 weight 파일만 보관하면 안 되는가

`best.onnx`만 있으면 어떤 data와 code로 만들었는지, 이전 model보다 좋은지, license가
무엇인지 알 수 없다. 장애 때 안전한 rollback도 불가능하다. model ID와 checksum, dataset,
평가, runtime, license를 함께 묶어야 software release처럼 검토·재현할 수 있다.

각 배포 model에 다음 manifest를 둔다.

```yaml
model_id: phone-ui-detector-v1
task: detection
classes: [phone_screen, button, icon, text_field, toggle]
framework: pinned-name-and-version
source_commit: git-commit
dataset_version: phone-ui-2026-09-v1
input_size: 640
export: onnx-fp32
test_map50_95: measured-value
camera_test_hit_rate: measured-value
license: reviewed-license-and-obligations
```

weight를 `latest.onnx`로만 관리하지 않는다. model ID, checksum, dataset, 평가 결과와 rollback
대상을 함께 기록한다.

## 19. 단계별 완료 기준

### 왜 phase별 gate를 두는가

화면 탐지가 불안정한 상태에서 UI/OCR을 학습하면 하위 단계의 입력 자체가 흔들린다.
phase별 gate는 다음 단계가 의존하는 품질을 먼저 증명해 원인 불명의 복합 실패를 막는다.

| Phase | 이 단계가 먼저인 이유 | 다음 단계로 넘어가는 증거 |
| --- | --- | --- |
| A baseline | custom 학습 필요성과 처리 budget 확인 | 실제-camera class별 metric·latency |
| B rectification | 모든 UI model 입력 좌표를 안정화 | corner/round-trip error 기준 통과 |
| C UI+OCR | 실제 버튼 의미와 선택 정확도 검증 | hit rate, CER, unknown 정책 |
| D 통합 | model 단독 점수와 운영 stream 품질은 다름 | soak, viewer FPS, rollback, license |

### Phase A — 사람·휴대폰 pretrained baseline

- [ ] IMX708 test set 200장 이상 annotation
- [ ] person/cell phone precision·recall·mAP 기록
- [ ] 서버 CPU inference p95와 viewer FPS 기록
- [ ] threshold 확정
- [ ] custom training 필요 여부 결정

### Phase B — Phone screen rectification

- [ ] 실제 기기/각도/조명 데이터 수집
- [ ] screen mask/corner model test 성능 기록
- [ ] corner error와 rectified image 품질 확인
- [ ] 원본↔보정 좌표 round-trip unit test

### Phase C — UI element + OCR

- [ ] 최소 class taxonomy 확정
- [ ] device/app/session 분리 dataset
- [ ] UI element mAP와 click-point hit rate 기록
- [ ] 한국어/영어 OCR CER 기록
- [ ] unknown icon과 low-confidence 정책 구현

### Phase D — 시스템 통합

- [ ] `objects`, `phone_ui` mode 추가
- [ ] inference FPS와 display FPS 분리
- [ ] binary/metadata 경로로 Base64 제거
- [ ] 30분 soak test
- [ ] 개인정보/모델 license review
- [ ] model rollback 검증

## 20. 참고 자료

공식 문서와 원 연구 자료를 우선한다.

- [Ultralytics model training](https://docs.ultralytics.com/modes/train/)
- [Ultralytics model export](https://docs.ultralytics.com/modes/export/)
- [Ultralytics detection dataset format](https://docs.ultralytics.com/datasets/detect/)
- [Ultralytics license 안내](https://www.ultralytics.com/license)
- [CVAT YOLO dataset format](https://docs.cvat.ai/docs/dataset_management/formats/format-yolo/)
- [ONNX Runtime 개요](https://onnxruntime.ai/docs/)
- [ONNX Runtime model quantization](https://onnxruntime.ai/docs/performance/model-optimizations/quantization.html)
- [OpenCV homography와 perspective correction](https://docs.opencv.org/4.x/d9/dab/tutorial_homography.html)
- [Raspberry Pi AI HAT 공식 문서](https://www.raspberrypi.com/documentation/accessories/ai-hat-plus.html)
- [RICO mobile UI dataset](https://www.interactionmining.org/archive/rico)
- [Google RICO Semantics dataset](https://github.com/google-research-datasets/rico_semantics)
- [Towards Better Semantic Understanding of Mobile Interfaces](https://aclanthology.org/2022.coling-1.497/)
- [ScreenAI — Google Research](https://research.google/pubs/screenai-a-vision-language-model-for-ui-and-infographics-understanding/)
- [Spotlight — vision-only mobile UI understanding](https://research.google/pubs/spotlight-mobile-ui-understanding-using-vision-language-models-with-a-focus/)
- [ScreenSpot / SeeClick paper](https://openreview.net/forum?id=4b5f397355df51294432bb9e8d8641fe7f3426c1)
