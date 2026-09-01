# Server / Client 코드 학습 시작 가이드

## 1. 이 문서의 목표

이 프로젝트를 처음 읽는 사람이 다음 질문에 답할 수 있게 하는 것이 목표다.

- Raspberry Pi 카메라 frame이 어떤 순서로 브라우저까지 가는가?
- TypeScript/NestJS server에서 각 class와 decorator는 왜 필요한가?
- FastAPI/OpenCV가 server와 분리된 이유는 무엇인가?
- C++ client가 카메라 subprocess, thread, queue와 Socket.IO를 어떻게 연결하는가?
- frame이 느릴 때 왜 쌓지 않고 버리는가?
- 기능을 수정할 때 어떤 파일부터 보고 어디를 테스트해야 하는가?

TypeScript와 C++을 완전히 배운 뒤 코드를 볼 필요는 없다. 아래 순서대로 “작은 타입 →
상태 → 입출력 → 전체 조립”으로 읽으면서 필요한 문법을 함께 학습한다.

## 2. 가장 먼저 볼 전체 구조

```text
Raspberry Pi
┌─────────────────────────────────────────────────────────┐
│ rpicam-vid                                               │
│   └─ MJPEG stdout                                        │
│        └─ CameraProcess (C++)                            │
│             └─ LatestFrameQueue(capacity=1)              │
│                  └─ SocketIoClient (C++)                 │
└──────────────────────────┬──────────────────────────────┘
                           │ JPEG binary / Socket.IO
                           ▼
Mac / Docker
┌─────────────────────────────────────────────────────────┐
│ CameraGateway (NestJS/TypeScript)                        │
│   └─ CameraService                                      │
│        └─ OpenCvService ── HTTP multipart ──► FastAPI    │
│                                              └─ OpenCV   │
│   ◄──────── processed JPEG + detection metadata ─────────│
│   └─ viewer:frame / Socket.IO                            │
└──────────────────────────┬──────────────────────────────┘
                           ▼
Browser public/app.js → Blob URL → <img>
```

### 왜 이 그림을 먼저 보는가

한 파일씩 무작정 열면 같은 `frame`이 C++ vector, Socket.IO binary, Node Buffer, Python
NumPy array, Base64 문자열, browser Blob으로 계속 바뀌는 이유를 놓치기 쉽다. 먼저 process
경계와 data 형식을 기억하면 각 변환의 비용과 책임을 이해할 수 있다.

## 3. 권장 학습 순서 요약

### Server 읽기 순서

1. `server/src/camera/camera.types.ts`
2. `server/src/app.module.ts`
3. `server/src/main.ts`
4. `server/src/health.controller.ts`
5. `server/src/camera/camera.service.ts`
6. `server/src/opencv/opencv.service.ts`
7. `server/src/camera/camera.gateway.ts`
8. `server/vision/app.py`
9. `server/public/app.js`
10. `server/test/camera.service.spec.ts`

### Client 읽기 순서

1. `client/include/camera_client/config.hpp`
2. `client/src/config.cpp`
3. `client/include/camera_client/frame.hpp`
4. `client/include/camera_client/latest_frame_queue.hpp`
5. `client/include/camera_client/camera_process.hpp`
6. `client/src/camera_process.cpp`
7. `client/include/camera_client/socket_io_client.hpp`
8. `client/src/socket_io_client.cpp`
9. `client/include/camera_client/application.hpp`
10. `client/src/application.cpp`
11. `client/src/main.cpp`
12. `client/tests/test_main.cpp`
13. `client/Makefile`
14. `client/packaging/camera-client.service`

이 순서는 실행 순서와 완전히 같지 않다. 학습할 때는 작은 data type과 interface를 먼저 본
뒤 복잡한 비동기/network 구현을 보는 편이 이해하기 쉽다.

## 4. TypeScript를 읽기 위한 최소 기초

### 4.1 TypeScript와 JavaScript의 관계

TypeScript는 JavaScript에 type 검사 문법을 더한 언어다. `interface`, type annotation,
generic 같은 정보는 compile 후 대부분 사라지고 Node.js는 JavaScript를 실행한다.

```ts
const port: number = 3000;
```

compile된 개념적 JavaScript:

```js
const port = 3000;
```

**왜 type을 쓰는가:** 실행 전에 잘못된 property, 인자와 반환값을 compiler가 찾아준다.
단, 외부 JSON이 실제로 interface와 같은지는 자동 검사하지 않으므로 runtime validation은
별도 문제다.

### 4.2 자주 나오는 문법

| 문법 | 의미 | 프로젝트 예 |
| --- | --- | --- |
| `name: string` | 변수/property type | `cameraId: string` |
| `value?: number` | 없어도 되는 optional 값 | `timestamp?: number` |
| `A \| B` | 둘 중 하나인 union | `Buffer \| Uint8Array` |
| `T[]` | T의 배열 | `Detection[]` |
| `Promise<T>` | 나중에 T로 완료되는 비동기 결과 | `Promise<ProcessedFrame>` |
| `value ?? fallback` | null/undefined일 때 기본값 | `PORT ?? 3000` |
| `object?.field` | object가 없으면 예외 대신 undefined | `existing?.lastFrameAt` |
| `value!` | 값이 있다고 compiler에 단언 | `states.get(id)!` |
| `value as T` | compile-time type assertion | auth role, JSON result |
| `{ ...value }` | object shallow copy | camera state 반환 |
| `[...iterator]` | iterable을 배열로 펼침 | Map values |
| `` `${value}` `` | template literal | URL/log 문자열 |

### 4.3 `async`, `await`, Promise

```ts
async function load(): Promise<Result> {
  const response = await fetch(url);
  return response.json();
}
```

- `async` 함수는 항상 Promise를 반환한다.
- `await`는 Promise가 완료될 때까지 해당 함수의 다음 줄만 기다린다.
- Node.js process 전체 thread를 sleep시키는 것과 다르다.
- Promise가 reject되면 `try/catch`에서 잡거나 caller로 전파된다.

**이 프로젝트에서 중요한 이유:** vision HTTP 요청은 비동기지만 같은 camera가 처리 중인
동안 다음 frame을 쌓지 않고 drop한다. 비동기라고 해서 처리량이 무한해지는 것은 아니다.

### 4.4 ESM import/export

```ts
import { CameraService } from './camera.service.js';
export class CameraGateway {}
```

- `export`한 이름만 다른 파일에서 import할 수 있다.
- `import type`은 compile-time type에만 사용한다.
- source는 `.ts`지만 ESM 실행 파일이 `.js`이므로 상대 import에 `.js`를 쓴다.

### 4.5 class 접근 제한자

- `public`: 외부에서 접근 가능, 생략 시 기본값
- `private`: class 내부만 접근
- `readonly`: 생성 후 다른 값으로 재할당 금지

Constructor parameter property:

```ts
constructor(private readonly cameras: CameraService) {}
```

다음 세 동작을 줄인 문법이다.

```ts
private readonly cameras: CameraService;
constructor(cameras: CameraService) {
  this.cameras = cameras;
}
```

## 5. NestJS를 읽기 위한 최소 기초

### 5.1 Module

`AppModule`은 controller와 provider 목록을 NestJS에 알려주는 root 구성이다.

```ts
@Module({
  controllers: [HealthController],
  providers: [CameraGateway, CameraService, OpenCvService],
})
```

### 5.2 Decorator

`@이름(...)`은 class/method/parameter에 framework metadata를 붙인다.

| Decorator | 역할 |
| --- | --- |
| `@Module` | module 구성 |
| `@Injectable` | DI provider 등록 가능 표시 |
| `@Controller('api')` | HTTP route prefix |
| `@Get('health')` | GET method route |
| `@WebSocketGateway` | Socket.IO gateway |
| `@SubscribeMessage` | Socket.IO event handler |
| `@ConnectedSocket` | event 송신 socket 주입 |
| `@MessageBody` | event payload 주입 |

### 5.3 Dependency Injection(DI)

객체가 dependency를 직접 `new`하지 않고 constructor로 요청한다.

```text
NestJS container
├─ OpenCvService 생성
├─ CameraService(OpenCvService) 생성
├─ CameraGateway(CameraService) 생성
└─ HealthController(CameraService, OpenCvService) 생성
```

**왜 사용하는가:** 객체 생성 책임과 업무 logic을 분리하고 test에서 fake dependency를 넣기
쉽게 한다. `CameraService` test가 실제 FastAPI 없이 동작하는 이유도 constructor에 fake
`OpenCvService`를 넣을 수 있기 때문이다.

## 6. Server 코드 상세 읽기 순서

### 6.1 1단계 — `camera.types.ts`

먼저 시스템을 오가는 data 모양을 익힌다.

- `ClientRole`: camera/viewer 권한 구분
- `IncomingFrame`: Pi가 보내는 payload
- `Detection`: motion/object box 공통 형식
- `ProcessedFrame`: vision 처리 결과
- `CameraState`: 연결과 누적 통계

읽으며 답할 질문:

1. timestamp 단위는 무엇인가?
2. binary frame은 어떤 세 타입으로 들어올 수 있는가?
3. 아직 frame이 없는 camera의 `lastFrameAt`은 왜 null인가?

### 6.2 2단계 — `app.module.ts`

어떤 class가 HTTP controller이고 어떤 class가 provider인지 본다. class body가 비어 있어도
`@Module` metadata가 실제 wiring 역할을 한다.

### 6.3 3단계 — `main.ts`

Process가 시작되는 파일이다.

```text
dotenv load
→ NestFactory.create(AppModule)
→ shutdown/CORS/static assets 설정
→ PORT 결정
→ 0.0.0.0 listen
```

여기서 배울 이론:

- process 환경 변수
- server bind address와 port
- CORS
- static file serving
- graceful shutdown

### 6.4 4단계 — `health.controller.ts`

가장 단순한 요청/응답으로 Nest controller를 익힌다.

```text
GET /api/health → OpenCvService.health() → JSON
GET /api/cameras → CameraService.list() → JSON
```

`return`한 object를 NestJS가 JSON response로 바꾼다는 점을 확인한다.

### 6.5 5단계 — `camera.service.ts`

Camera별 상태와 backpressure 핵심이다.

자료구조:

- `Map<cameraId, CameraState>`: 상태
- `Set<cameraId>`: 현재 처리 중 여부
- `Map<cameraId, timestamp>`: 마지막 처리 허용 시각

Frame 승인 판단:

```text
frame 도착
  ├─ 처리 중? ───────────────► drop
  ├─ minimum interval 미만? ─► drop
  └─ 아니면 processing 등록
       └─ OpenCV await
            └─ finally에서 processing 해제
```

**왜 finally가 중요한가:** vision 요청이 실패해도 Set에서 cameraId를 삭제해야 다음 frame을
받을 수 있다.

### 6.6 6단계 — `opencv.service.ts`

NestJS와 Python service 사이 adapter다.

```text
Node Buffer
→ Uint8Array
→ Blob
→ FormData multipart
→ fetch POST
→ JSON + Base64
→ Node Buffer
```

이 파일을 읽을 때 binary와 Base64의 차이, HTTP status와 fetch exception의 차이,
`AbortSignal.timeout`을 공부한다. 현재 성능 TODO에서 Base64 제거가 높은 우선순위인 이유도
이 변환 흐름에서 보인다.

### 6.7 7단계 — `camera.gateway.ts`

가장 마지막에 읽는 TypeScript 파일이다. 인증, event, binary, ACK와 broadcast가 모두
모여 있다.

연결 흐름:

```text
Socket.IO handshake auth
  ├─ role=camera
  │    ├─ cameraId 정규식 검사
  │    ├─ CAMERA_TOKEN timing-safe 비교
  │    └─ camera online broadcast
  └─ role=viewer
       ├─ VIEWER_TOKEN 검사
       └─ camera list 전송
```

Frame 흐름:

```text
camera:frame
→ role 확인
→ Buffer 정규화
→ size/JPEG magic byte 확인
→ CameraService.acceptFrame
→ viewer:frame broadcast
→ camera ACK
```

### 6.8 8단계 — `server/vision/app.py`

TypeScript는 아니지만 영상 처리의 실제 내용을 이해하려면 필요하다.

공부할 개념:

- FastAPI route와 multipart upload
- `await frame.read()`
- NumPy byte view
- OpenCV JPEG decode/encode
- grayscale, Gaussian blur, frame difference
- threshold, dilation, contour와 bounding box
- Base64 JSON 변환

Motion detection 원리:

```text
현재 frame grayscale/blur
  - 이전 frame grayscale/blur
→ 차이 절댓값
→ threshold로 움직인 pixel만 흰색
→ dilation으로 작은 영역 연결
→ contour/bounding box
```

### 6.9 9단계 — `server/public/app.js`

Browser가 viewer Socket.IO로 접속해 `viewer:frame`을 받는다.

핵심 개념:

- DOM query와 event listener
- Socket.IO browser client
- ArrayBuffer/Blob
- Object URL 생성과 폐기
- `<img>.src` 갱신
- `performance.now()` 기반 viewer FPS

### 6.10 10단계 — `camera.service.spec.ts`

Test가 구현 의도를 가장 짧게 보여준다.

- 연결 상태가 true/false로 변하는가?
- 첫 frame이 처리 중일 때 두 번째 frame을 null/drop 처리하는가?
- 실제 OpenCV 없이 Promise를 직접 제어하는 fake를 어떻게 만드는가?

## 7. WebSocket과 Socket.IO 기초

### 7.1 일반 HTTP와 WebSocket

HTTP는 일반적으로 client 요청 하나에 server 응답 하나다. WebSocket은 한 번 연결한 뒤
양쪽이 필요할 때 message를 보낼 수 있어 실시간 frame에 적합하다.

### 7.2 Socket.IO는 WebSocket과 같지 않다

Socket.IO는 WebSocket 위에 다음 기능을 추가한 protocol/library다.

- Engine.IO handshake와 ping/pong
- namespace
- 이름 있는 event
- acknowledgement(ACK)
- binary attachment
- reconnect

따라서 C++ client가 WebSocket만 연결하고 JPEG를 보내면 NestJS Socket.IO가
`camera:frame` event로 이해하지 못한다.

### 7.3 이 프로젝트의 event

| 방향 | Event | 내용 |
| --- | --- | --- |
| Camera → Server | `camera:frame` | timestamp + JPEG binary |
| Server → Camera | ACK | accepted + optional reason |
| Server → Viewer | `camera:list` | 초기 camera 목록 |
| Server → Viewer | `camera:status` | online/offline |
| Server → Viewer | `viewer:frame` | 처리 JPEG + detection |

## 8. Backpressure와 latest-frame 원리

Camera가 10 FPS로 frame을 만들지만 server가 8 FPS만 처리하면 매초 2 frame이 남는다.
일반 queue에 계속 넣으면 10초 후 20 frame, 1분 후 120 frame이 밀려 사용자는 과거 영상을
보게 되고 memory도 증가한다.

이 프로젝트의 선택:

```text
old waiting frame + new frame → old frame 버림 → new frame만 보관
```

**왜 재전송하지 않는가:** 실시간 camera에서는 모든 frame 전달보다 가장 최신 장면을 빨리
보는 것이 중요하다. `backpressure`는 network 오류가 아니라 의도된 latency 제어다.

## 9. C++20을 읽기 위한 최소 기초

### 9.1 Header와 source

- `.hpp`: class/function의 공개 선언
- `.cpp`: 실제 구현

먼저 header에서 책임과 입력/반환을 읽고 source에서 system call과 알고리즘을 본다.

### 9.2 Namespace

```cpp
namespace camera_client { ... }
```

다른 library와 같은 이름이 충돌하지 않게 project 이름 공간을 만든다.

### 9.3 RAII

Resource Acquisition Is Initialization. 객체 수명과 file/socket/thread 같은 resource 수명을
묶는다. Destructor에서 `stop()`/`disconnect()`를 호출하면 exception이나 early return에도
정리 가능성이 높아진다.

### 9.4 값, 참조, move

```cpp
void push(Frame frame);
queue.push(std::move(frame));
```

JPEG vector는 크므로 복사보다 ownership을 옮기는 move가 효율적이다. `std::move` 이후 원래
객체의 값에 의존하지 않는다.

### 9.5 스마트 포인터와 PImpl

`SocketIoClient`는 `std::unique_ptr<Impl>`을 사용한다.

- unique_ptr: 한 owner만 가지는 heap object
- PImpl: Boost.Beast 같은 큰 구현 type을 header에서 숨김
- header compile dependency와 public API 변경 범위를 줄임

### 9.6 Thread 기초

- `std::jthread`: 종료 때 자동 join, stop token 지원
- `std::mutex`: 동시에 공유 data 접근 방지
- `std::lock_guard`/`unique_lock`: scope 끝에서 mutex 자동 해제
- `std::condition_variable`: busy loop 없이 새 frame을 기다림
- `std::atomic_bool`: signal/main thread 사이 stop flag

### 9.7 시간

- `system_clock`: 실제 epoch timestamp
- `steady_clock`: 시스템 시간이 바뀌어도 역행하지 않는 interval/backoff

Frame timestamp와 sleep/reconnect 시계를 분리하는 이유다.

## 10. Client 코드 상세 읽기 순서

### 10.1 1~2단계 — `config.hpp`, `config.cpp`

먼저 실행 입력과 validation을 이해한다.

```text
CLI > environment variable > default
```

배울 내용:

- struct와 default member value
- `std::optional`
- `std::getenv`
- `std::from_chars` 반환 error
- `std::string_view`
- exception을 이용한 잘못된 설정 보고

질문:

1. token이 log/error에 포함되지 않는가?
2. FPS/quality 범위는 어디서 검사하는가?
3. camera ID 정규식 규칙과 server 규칙이 같은가?

### 10.2 3단계 — `frame.hpp`

가장 단순한 data object다.

- timestamp: `std::int64_t`
- JPEG: `std::vector<std::uint8_t>`

왜 raw RGB가 아니라 JPEG를 queue에 두는지 생각한다. JPEG가 훨씬 작아 thread 사이 복사와
network 비용을 줄인다.

### 10.3 4단계 — `latest_frame_queue.hpp`

Thread-safe capacity-1 queue다.

```text
capture thread → push
network thread → pop_for
```

배울 내용:

- mutex로 critical section 보호
- optional로 frame 존재 여부 표현
- condition variable predicate
- timeout과 close
- move semantics

### 10.4 5~6단계 — `camera_process.hpp/.cpp`

Linux process와 pipe를 처음 접하면 호출 흐름부터 본다.

```text
pipe()
→ fork()
   ├─ child: dup2(stdout) → execlp(rpicam-vid)
   └─ parent: read(pipe)
        → FF D8 / FF D9 marker 탐색
        → Frame 생성
```

필수 이론:

- process와 thread 차이
- file descriptor
- anonymous pipe
- parent/child process
- `fork` 반환값: -1 error, 0 child, 양수 parent의 child PID
- `exec` 성공 시 돌아오지 않음
- `read`: 양수 byte, 0 EOF, -1 error
- signal과 `waitpid` zombie 방지
- MJPEG는 JPEG들이 이어진 byte stream이라는 점

### 10.5 7~8단계 — `socket_io_client.hpp/.cpp`

Client에서 가장 어려운 파일이므로 마지막 쪽에 읽는다.

층:

```text
TCP
└─ WebSocket (Boost.Beast)
   └─ Engine.IO v4
      └─ Socket.IO namespace/event/ACK
```

읽을 순서:

1. URL parser
2. TCP resolve/connect
3. WebSocket handshake
4. Engine.IO open packet
5. `/stream` auth packet
6. binary event descriptor
7. JPEG binary attachment
8. ACK parse
9. ping/pong
10. disconnect

Network byte protocol은 주석과 server gateway를 나란히 놓고 본다.

### 10.6 9~10단계 — `application.hpp/.cpp`

모든 component의 orchestration이다.

```text
camera thread start
→ frame queue push
→ server connect
→ latest frame pop
→ send + ACK
→ 통계
→ error 시 exponential backoff
→ stop 시 queue/socket/camera 순서대로 정리
```

배울 내용:

- callback/lambda capture `[&]`
- atomic counter
- exception boundary
- exponential backoff + jitter
- cleanup order

### 10.7 11단계 — `main.cpp`

Process 시작점이다.

- `--help` 확인
- SIGINT/SIGTERM handler 등록
- config parse
- Application 생성/실행
- exit code 반환

Signal handler에서는 atomic flag만 변경하고 socket close나 logging을 하지 않는 이유를
확인한다. 많은 library 함수는 signal context에서 안전하지 않다.

### 10.8 12단계 — `tests/test_main.cpp`

- 최신 frame 교체
- CLI가 environment보다 우선
- 잘못된 camera ID 거부

Test를 먼저 읽고 구현을 보면 함수가 보장하려는 계약이 명확해진다.

### 10.9 13~14단계 — `Makefile`, systemd unit

Makefile:

```text
.cpp → compile → .o → link → executable
```

- `CPPFLAGS`: include 경로
- `CXXFLAGS`: C++20, 최적화, warning
- `LDFLAGS`: linker option
- `LDLIBS`: Boost.System, pthread
- `.d`: header dependency 자동 추적

Systemd unit:

- network-online 뒤 시작
- `pi`/`video` 권한
- `/etc/camera-client.env` 설정
- 실패 시 5초 후 재시작
- `/opt/camera-client/bin/camera-client` 실행

## 11. End-to-end 호출 순서

### 11.1 시작

```text
systemd 또는 shell
→ client main()
→ parse_config()
→ Application.run()
→ CameraProcess.start()
→ fork/exec rpicam-vid
→ SocketIoClient.connect()
→ server CameraGateway.handleConnection()
→ CameraService.connect()
→ viewer camera:status
```

### 11.2 Frame 한 장

```text
rpicam JPEG bytes
→ CameraProcess parser
→ Frame(timestamp, jpeg)
→ LatestFrameQueue.push(move)
→ Application.pop_for()
→ SocketIoClient.send_frame()
→ CameraGateway.onFrame()
→ CameraService.acceptFrame()
→ OpenCvService.process()
→ FastAPI process()
→ motion detection + JPEG encode
→ CameraGateway viewer:frame
→ browser renderFrame()
→ camera ACK
```

### 11.3 종료

```text
Ctrl+C / SIGTERM
→ atomic stop flag=true
→ Application loop 종료
→ queue.close()
→ SocketIoClient.disconnect()
→ CameraProcess.stop()
→ SIGTERM child + waitpid + thread join
→ process exit 0
```

## 12. 추천 실습 순서

### 실습 1 — 읽기 전용 상태 확인

```bash
curl http://192.168.45.89:3000/api/health
curl http://192.168.45.89:3000/api/cameras
```

응답 field를 `HealthController`와 `CameraState`에서 찾아본다.

### 실습 2 — 설정 validation

```bash
cd client
./build/camera-client --help
./build/camera-client --fps 0
```

어떤 함수가 오류를 만들고 main이 어떤 exit code를 반환하는지 추적한다. 두 번째 명령은
필수 server/token 설정에 따라 다른 설정 오류가 먼저 나올 수 있다.

### 실습 3 — Backpressure 관찰

Client를 실행하고 `/api/cameras`를 여러 번 조회한다.

```bash
watch -n 1 'curl -s http://192.168.45.89:3000/api/cameras'
```

`receivedFrames`, `droppedFrames` 변화와 `CameraService.acceptFrame` 조건을 연결한다.

### 실습 4 — Test로 계약 확인

```bash
cd server
npm run typecheck
npm test

cd ../client
make test
```

Test 이름을 읽고 구현에서 해당 조건을 찾는다.

### 실습 5 — 안전한 작은 변경

먼저 log나 새 read-only health field처럼 protocol을 깨지 않는 변경을 한다.

1. type/interface 수정
2. 구현 수정
3. test 수정
4. typecheck/test/build
5. 실제 API 확인
6. 문서 갱신

## 13. 증상별 먼저 볼 파일

| 증상 | 첫 파일 | 다음 확인 |
| --- | --- | --- |
| Server가 안 뜸 | `server/src/main.ts` | Docker log, PORT |
| `/api/health` degraded | `health.controller.ts` | `opencv.service.ts`, vision health |
| Camera `connected:false` | `camera.gateway.ts` | token, client process |
| Frame drop 많음 | `camera.service.ts` | vision processing, MAX_CAMERA_FPS |
| `processing_failed` | `opencv.service.ts` | `vision/app.py`, container log |
| Browser 화면 없음 | `public/app.js` | camera list/status/frame event |
| Client camera 시작 실패 | `camera_process.cpp` | rpicam, video 권한 |
| Client 연결 실패 | `socket_io_client.cpp` | URL, token, server port |
| Memory/지연 증가 | `latest_frame_queue.hpp` | queue 교체/worker 구조 |
| 종료가 느림 | `application.cpp` | socket timeout, child wait/join |

## 14. 수정 영향 범위 지도

### 새 detection field 추가

```text
camera.types.ts
→ vision/app.py response
→ opencv.service.ts VisionResponse
→ camera.gateway.ts viewer payload
→ public/app.js rendering
→ tests/docs
```

### 새 client 설정 추가

```text
config.hpp Config
→ config.cpp env/CLI/default/validation
→ 사용 component
→ tests
→ env example
→ README/install.md/systemd 설명
```

### Socket.IO protocol 변경

```text
server camera.gateway.ts
↔ client socket_io_client.cpp
→ browser app.js(필요 시)
→ protocol docs/integration tests
```

Server와 client 한쪽만 바꾸면 연결은 되어 보여도 event/ACK가 깨질 수 있다.

## 15. 용어 사전

| 용어 | 쉬운 설명 |
| --- | --- |
| Frame | 카메라 영상의 한 장 |
| JPEG | 한 장의 이미지를 압축한 binary 형식 |
| MJPEG | 여러 JPEG frame이 연속된 stream |
| FPS | 1초당 frame 수 |
| Latency | 촬영부터 표시까지 걸린 시간 |
| Backpressure | 느린 소비자가 감당 못할 때 입력을 제한하는 방식 |
| DI | 필요한 객체를 framework가 constructor에 넣어주는 방식 |
| Decorator | class/method에 framework metadata를 붙이는 문법 |
| Promise | 미래에 완료/실패할 비동기 결과 |
| Buffer | Node.js binary byte container |
| Blob | Browser/Node web API의 binary object |
| WebSocket | 연결을 유지하는 양방향 message protocol |
| Engine.IO | Socket.IO 아래의 연결/ping transport protocol |
| Socket.IO | event/namespace/ACK를 제공하는 실시간 protocol/library |
| ACK | message 처리 결과를 sender에게 돌려주는 응답 |
| Namespace | Socket.IO 연결을 논리적으로 분리한 endpoint |
| RAII | C++ 객체 수명과 resource 정리를 묶는 방식 |
| Move | 큰 data를 복사하지 않고 ownership을 이전하는 방식 |
| Mutex | 여러 thread가 공유 data를 동시에 바꾸지 못하게 하는 lock |
| Condition variable | 조건이 될 때까지 thread를 효율적으로 대기시키는 도구 |
| Homography | 한 평면 좌표를 다른 평면 좌표로 바꾸는 3×3 변환 |

## 16. 학습 완료 체크리스트

- [ ] 전체 architecture를 그림 없이 말로 설명할 수 있다.
- [ ] `camera.types.ts`의 각 타입이 어느 process 경계를 지나는지 안다.
- [ ] NestJS module/controller/provider/gateway 차이를 설명할 수 있다.
- [ ] `async/await`와 backpressure가 서로 다른 문제임을 안다.
- [ ] 일반 WebSocket만으로 Socket.IO server에 전송할 수 없는 이유를 안다.
- [ ] CameraService가 frame을 drop하는 두 조건을 안다.
- [ ] FastAPI 경로의 JPEG/Base64 변환 순서를 안다.
- [ ] C++ latest-frame queue가 capacity 1인 이유를 안다.
- [ ] `fork/exec/pipe/read/waitpid`의 역할을 설명할 수 있다.
- [ ] RAII, move, jthread, mutex와 atomic의 기본 목적을 안다.
- [ ] 설정 변경과 protocol 변경의 영향 파일을 찾을 수 있다.
- [ ] server/client test 명령을 직접 실행할 수 있다.

## 17. 함께 읽을 프로젝트 문서

- [`server/README.md`](../server/README.md): server 실행과 protocol
- [`client/README.md`](../client/README.md): C++ client 설계·검증
- [`client/install.md`](../client/install.md): 빌드, systemd, 수정·배포
- [`TODO.md`](../TODO.md): 현재 성능과 개선 우선순위
- [`vision-object-ui-training-guide.md`](vision-object-ui-training-guide.md): 객체·UI 학습
- [`remote-ssh-connect.md`](remote-ssh-connect.md): SSH 공유 연결 원리
