// Controller는 class를 HTTP controller로, Get은 method를 GET route로 등록하는 decorator다.
import { Controller, Get } from '@nestjs/common';

// HTTP controller가 직접 상태를 저장하지 않고 service에 요청하도록 두 class를 import한다.
import { CameraService } from './camera/camera.service.js';
import { OpenCvService } from './opencv/opencv.service.js';

// class 아래의 모든 route 앞에 `/api` prefix를 붙인다.
@Controller('api')
export class HealthController {
  // constructor parameter에 접근 제한자(`private`)를 쓰면 TypeScript가 같은 이름의
  // class field를 자동 생성하고 받은 값을 저장한다. `readonly`는 재할당을 막는다.
  constructor(
    // NestJS DI가 providers에 등록된 CameraService 인스턴스를 전달한다.
    private readonly cameras: CameraService,
    // 같은 방식으로 OpenCvService 인스턴스를 전달한다.
    private readonly openCv: OpenCvService,
  ) {}

  // `@Get('health')`는 이 method를 GET `/api/health` handler로 등록한다.
  @Get('health')
  // `async` 함수는 항상 Promise를 반환한다.
  // `Record<string, unknown>`은 문자열 key를 가지되 값 타입을 미리 제한하지 않은 객체다.
  async health(): Promise<Record<string, unknown>> {
    // `await`는 Promise가 완료될 때까지 이 함수의 다음 줄만 일시 중지한다.
    // OpenCvService.health() 반환값은 vision service가 정상이면 true인 boolean이다.
    const vision = await this.openCv.health();

    // 객체를 return하면 NestJS가 JSON으로 직렬화하고 HTTP response body에 쓴다.
    return {
      // 삼항 연산자 `조건 ? A : B`는 조건이 참이면 A, 거짓이면 B를 선택한다.
      status: vision ? 'ok' : 'degraded',

      // property shorthand: `vision: vision`을 `vision` 하나로 줄인 문법이다.
      vision,

      // process.uptime()은 Node.js process 실행 시간을 초 단위 실수로 반환한다.
      // Math.floor()는 소수점 아래를 버린 정수를 반환한다.
      uptimeSeconds: Math.floor(process.uptime()),

      // new Date()는 현재 시각 객체, toISOString()은 UTC ISO-8601 문자열을 반환한다.
      timestamp: new Date().toISOString(),
    };
  }

  // 이 method는 GET `/api/cameras` 요청을 처리한다.
  @Get('cameras')
  // `unknown[]`은 아직 구체 타입을 선언하지 않은 값들의 배열을 의미한다.
  // 실제 반환은 CameraState[]이며 이후에는 그 타입으로 좁히는 것이 더 좋다.
  camerasList(): unknown[] {
    // list()는 내부 Map을 외부에서 안전하게 읽을 수 있는 새 객체 배열로 반환한다.
    return this.cameras.list();
  }
}
