// Vitest의 test 구성 함수와 mock/검증 도구를 named import한다.
import { beforeEach, describe, expect, it, vi } from 'vitest';

// 테스트 대상인 실제 CameraService class를 가져온다.
import { CameraService } from '../src/camera/camera.service.js';

// OpenCvService는 fake object의 타입 설명에만 사용한다.
import type { OpenCvService } from '../src/opencv/opencv.service.js';

// describe()는 관련 test case를 `CameraService`라는 suite로 묶는다.
describe('CameraService', () => {
  // beforeEach() callback은 각 `it` test가 시작하기 전에 실행돼 test 간 환경을 맞춘다.
  beforeEach(() => {
    // 환경 변수는 문자열이다. 높은 FPS로 설정해 일반 test가 시간 gate에 걸릴 가능성을 줄인다.
    process.env.MAX_CAMERA_FPS = '1000';
  });

  // it(name, callback)은 하나의 독립된 기대 동작을 정의한다.
  it('tracks connected cameras', () => {
    // 이 test는 vision process를 호출하지 않으므로 빈 객체를 OpenCvService처럼 주입한다.
    // `as`는 test double 작성 편의를 위한 type assertion이며 실제 객체 변환은 아니다.
    const service = new CameraService({} as OpenCvService);

    // connect() 반환값은 void이고 내부 camera state를 생성한다.
    service.connect('pi-1');

    // expect(actual)는 assertion wrapper를 반환한다.
    // toMatchObject()는 실제 객체가 주어진 property/value를 포함하는지 검사한다.
    expect(service.list()[0]).toMatchObject({ cameraId: 'pi-1', connected: true });

    service.disconnect('pi-1');

    // optional chaining `?.`은 배열이 비었어도 TypeError 대신 undefined를 만든다.
    // toBe(false)는 strict equality로 false인지 검사한다.
    expect(service.list()[0]?.connected).toBe(false);
  });

  // async test callback은 Promise를 반환하며 Vitest는 resolve/reject될 때까지 기다린다.
  it('drops a second frame while the first is processing', async () => {
    // 첫 번째 가짜 vision Promise를 test가 원하는 시점에 끝내기 위해 resolve 함수를 보관한다.
    let resolveProcess: ((value: unknown) => void) | undefined;

    // vi.fn()은 호출 횟수/인자를 검사할 수 있는 mock 함수를 반환한다.
    // 새 Promise의 executor가 받은 resolve를 바깥 변수에 저장해 일부러 pending 상태로 둔다.
    const process = vi.fn(
      () =>
        new Promise((resolve) => {
          resolveProcess = resolve;
        }),
    );

    // `{ process }`는 `{ process: process }` shorthand다.
    // object 전체가 완전한 OpenCvService가 아니므로 unknown을 거쳐 test double로 assertion한다.
    const service = new CameraService({ process } as unknown as OpenCvService);
    service.connect('pi-1');

    // await하지 않아 첫 번째 frame은 processing Set에 남고 Promise가 pending 상태가 된다.
    const first = service.acceptFrame('pi-1', Buffer.from('frame'));

    // 두 번째 호출은 첫 번째 처리 중이므로 즉시 null로 resolve되어야 한다.
    const second = await service.acceptFrame('pi-1', Buffer.from('frame'));

    // toBeNull()은 반환값이 정확히 null인지 검사한다.
    expect(second).toBeNull();
    expect(service.list()[0]?.droppedFrames).toBe(1);

    // optional call `?.(...)`은 resolveProcess가 존재할 때만 호출한다.
    // 이 가짜 결과가 첫 번째 acceptFrame의 await를 풀어준다.
    resolveProcess?.({
      cameraId: 'pi-1',
      // Buffer.alloc(1)은 0으로 채운 길이 1의 Node Buffer를 반환한다.
      frame: Buffer.alloc(1),
      detections: [],
      timestamp: 1,
      processingMs: 1,
    });

    // 첫 Promise 완료와 CameraService finally cleanup까지 기다린 뒤 test를 끝낸다.
    await first;
  });
});
