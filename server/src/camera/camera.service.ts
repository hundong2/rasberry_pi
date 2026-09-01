// Injectable decorator는 이 class를 NestJS DI container가 만들 수 있는 provider로 표시한다.
import { Injectable } from '@nestjs/common';

// 실제 OpenCV/FastAPI HTTP 호출은 별도 service에 위임한다.
import { OpenCvService } from '../opencv/opencv.service.js';

// 두 interface는 타입 검사에만 필요하므로 `import type`으로 가져온다.
import type { CameraState, ProcessedFrame } from './camera.types.js';

// @Injectable() metadata 덕분에 CameraGateway/HealthController constructor에 주입할 수 있다.
@Injectable()
export class CameraService {
  // Map<K,V>는 key로 value를 빠르게 찾는 자료구조다. cameraId별 상태를 보관한다.
  // `readonly`는 Map 변수 자체를 다른 Map으로 재할당하지 못하게 하지만 내부 set은 가능하다.
  private readonly states = new Map<string, CameraState>();

  // Set은 중복 없는 key 집합이다. 현재 vision 처리 중인 cameraId를 기록한다.
  private readonly processing = new Set<string>();

  // 마지막으로 처리 시작을 허용한 epoch millisecond를 camera별로 저장한다.
  private readonly lastAcceptedAt = new Map<string, number>();

  // NestJS가 OpenCvService 객체를 주입하며 private readonly field로 자동 저장한다.
  constructor(private readonly openCv: OpenCvService) {}

  /** camera를 online으로 표시하되 과거 누적 통계를 유지한다. */
  connect(cameraId: string): void {
    // Map.get()은 key가 있으면 CameraState, 없으면 undefined를 반환한다.
    const existing = this.states.get(cameraId);

    // Map.set()은 key의 값을 새 객체로 저장하고 Map 자신을 반환한다.
    this.states.set(cameraId, {
      // property shorthand로 parameter cameraId 값을 같은 이름 property에 넣는다.
      cameraId,
      connected: true,

      // `existing?.lastFrameAt`은 existing이 없으면 undefined다.
      // `?? null`은 새 camera에 아직 처리 frame이 없음을 null로 표현한다.
      lastFrameAt: existing?.lastFrameAt ?? null,

      // 재연결 때 통계를 0으로 초기화하지 않고 이전 값을 유지한다.
      receivedFrames: existing?.receivedFrames ?? 0,
      droppedFrames: existing?.droppedFrames ?? 0,
    });
  }

  /** camera를 offline으로 표시한다. unknown camera에는 아무 작업도 하지 않는다. */
  disconnect(cameraId: string): void {
    const state = this.states.get(cameraId);

    // JavaScript object는 참조 타입이므로 state.connected 변경이 Map 내부 객체에도 반영된다.
    if (state) state.connected = false;
  }

  /** 외부가 내부 Map 객체를 직접 수정하지 못하도록 shallow copy 배열을 반환한다. */
  list(): CameraState[] {
    // states.values()는 value iterator를 반환한다.
    // `[...iterator]` spread 문법은 iterator를 새 배열로 펼친다.
    // map()은 각 상태를 `{ ...state }`로 복사한 새 배열을 반환한다.
    return [...this.states.values()].map((state) => ({ ...state }));
  }

  /**
   * 한 frame을 FPS/backpressure 규칙으로 선별해 vision service로 처리한다.
   * @returns 처리 결과, 또는 정상 drop이면 null을 resolve하는 Promise.
   */
  async acceptFrame(
    cameraId: string,
    frame: Buffer,
    // default parameter: caller가 timestamp를 생략하면 호출 시점의 현재 시각을 사용한다.
    timestamp = Date.now(),
  ): Promise<ProcessedFrame | null> {
    const state = this.states.get(cameraId);

    // event가 connect 상태 등록보다 먼저/예외적으로 도착해도 상태를 자동 생성한다.
    if (!state) this.connect(cameraId);

    // 바로 위에서 없으면 connect했으므로 두 번째 get은 존재한다.
    // postfix `!`는 undefined가 아니라고 compiler에 알리는 non-null assertion이다.
    const current = this.states.get(cameraId)!;

    // `+= 1`은 기존 숫자에 1을 더해 다시 저장한다. drop 여부와 무관한 총 입력 수다.
    current.receivedFrames += 1;

    // 환경 값이 없으면 12 FPS다. Math.max(1, x)는 0 이하 FPS로 나누는 것을 막는다.
    const maxFps = Math.max(1, Number(process.env.MAX_CAMERA_FPS ?? 12));

    // 1000 ms를 FPS로 나누면 frame 처리 시작 사이의 최소 millisecond가 된다.
    const minimumInterval = 1_000 / maxFps;

    // Date.now()는 현재 Unix epoch millisecond 정수를 반환한다.
    const now = Date.now();

    // 마지막 기록이 없으면 0을 사용한다. 경과 시간이 최소 간격보다 작으면 tooFast=true다.
    const tooFast = now - (this.lastAcceptedAt.get(cameraId) ?? 0) < minimumInterval;

    // 같은 camera가 처리 중이거나 너무 빨리 왔다면 queue에 쌓지 않고 즉시 버린다.
    if (this.processing.has(cameraId) || tooFast) {
      current.droppedFrames += 1;
      return null;
    }

    // Set.add()는 cameraId를 처리 중 집합에 넣고 Set 자신을 반환한다.
    this.processing.add(cameraId);

    // 처리 허용 시각을 기록해 이후 입력의 FPS gate 기준으로 사용한다.
    this.lastAcceptedAt.set(cameraId, now);

    try {
      // OpenCvService.process()의 HTTP 요청/JSON 변환이 끝날 때까지 비동기로 기다린다.
      const processed = await this.openCv.process(cameraId, frame, timestamp);

      // vision 처리까지 성공한 시각만 lastFrameAt에 기록한다.
      current.lastFrameAt = Date.now();
      return processed;
    } finally {
      // finally는 성공/예외 모두 실행된다. 이 delete가 없으면 camera가 영원히 processing으로
      // 남아 이후 모든 frame이 drop된다. Set.delete()는 실제 삭제 여부 boolean을 반환한다.
      this.processing.delete(cameraId);
    }
  }
}
