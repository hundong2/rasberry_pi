// `import`는 다른 파일/패키지가 `export`한 값을 현재 파일에서 사용하게 한다.
// `Module`은 NestJS가 애플리케이션 구성 요소를 묶을 때 사용하는 decorator 함수다.
import { Module } from '@nestjs/common';

// `.js` 확장자를 쓰는 이유: 이 프로젝트는 TypeScript를 ESM JavaScript로 빌드한다.
// TypeScript는 개발 중 `.ts` 파일을 찾지만, 실행되는 Node.js는 빌드된 `.js`를 찾는다.
import { CameraGateway } from './camera/camera.gateway.js';
import { CameraService } from './camera/camera.service.js';
import { HealthController } from './health.controller.js';
import { OpenCvService } from './opencv/opencv.service.js';

// `@Module(...)`은 바로 아래 class에 NestJS module metadata를 붙이는 decorator다.
// decorator는 class 정의를 NestJS가 읽고 객체 생성/연결 방법을 알 수 있게 한다.
@Module({
  // `controllers`는 HTTP 요청 경로를 처리하는 class 목록이다.
  // NestJS가 HealthController 인스턴스를 직접 만들고 route를 등록한다.
  controllers: [HealthController],

  // `providers`는 NestJS DI(Dependency Injection) container가 생성·관리할 객체 목록이다.
  // 같은 provider가 constructor parameter로 필요하면 NestJS가 해당 인스턴스를 주입한다.
  providers: [CameraGateway, CameraService, OpenCvService],
})
// `export`는 다른 module(main.ts)이 AppModule을 import할 수 있게 공개한다.
// class body가 비어 있어도 위의 @Module metadata가 실제 설정 역할을 한다.
export class AppModule {}
