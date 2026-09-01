// side-effect import: 특정 이름을 가져오지 않고 package 초기화 코드만 실행한다.
// `dotenv/config`는 `.env` 값을 읽어 `process.env`에 넣는다.
import 'dotenv/config';

// NestFactory는 root module로부터 NestJS application 객체를 생성한다.
import { NestFactory } from '@nestjs/core';

// NestExpressApplication 타입을 사용하면 Express 전용 method(useStaticAssets 등)를
// TypeScript compiler가 알고 자동 완성과 타입 검사를 제공한다.
import { NestExpressApplication } from '@nestjs/platform-express';

// Node.js 내장 path module의 join()은 OS에 맞는 path separator로 경로를 합친다.
import { join } from 'node:path';

// AppModule은 controller/provider 전체 구성을 가진 root module이다.
import { AppModule } from './app.module.js';

// `Promise<void>`는 비동기 완료 여부만 나타내며 별도 결과값은 반환하지 않는다는 뜻이다.
async function bootstrap(): Promise<void> {
  // `await NestFactory.create<T>()`는 application 초기화가 끝난 뒤 app 객체를 반환한다.
  // `<NestExpressApplication>`은 generic type argument로 Express 기능을 사용한다고 알린다.
  const app = await NestFactory.create<NestExpressApplication>(AppModule, {
    // Socket.IO binary가 별도 경로로 처리되므로 Nest의 기본 HTTP body parser를 끈다.
    bodyParser: false,
  });

  // enableShutdownHooks()는 SIGTERM 같은 종료 신호를 Nest lifecycle에 연결한다.
  app.enableShutdownHooks();

  // enableCors()는 다른 origin의 browser가 API/WebSocket에 접근할 CORS header를 설정한다.
  // origin:true는 요청 origin을 허용하고 credentials:true는 credential 요청을 허용한다.
  app.enableCors({ origin: true, credentials: true });

  // process.cwd()는 Node process의 현재 작업 directory 문자열을 반환한다.
  // join(..., 'public')은 정적 HTML/CSS/JS directory 경로를 안전하게 만든다.
  app.useStaticAssets(join(process.cwd(), 'public'));

  // `??`는 왼쪽 값이 null/undefined일 때만 오른쪽 기본값을 사용한다.
  // environment variable은 문자열이므로 Number()로 port number로 변환한다.
  const port = Number(process.env.PORT ?? 3000);

  // listen()은 server socket bind가 성공하면 resolve되는 Promise를 반환한다.
  // `0.0.0.0`은 container/host의 모든 IPv4 interface에서 요청을 받겠다는 뜻이다.
  await app.listen(port, '0.0.0.0');

  // template literal(백틱)은 `${표현식}` 결과를 문자열 안에 삽입한다.
  // console.info()는 메시지를 stdout에 기록하고 의미 있는 반환값은 없다.
  console.info(`Camera viewer listening on http://0.0.0.0:${port}`);
}

// async 함수를 호출하면 Promise가 반환된다. `void` operator는 이 시작점에서 반환값을
// 의도적으로 사용하지 않는다는 것을 linter/type reader에게 명확히 한다.
void bootstrap();
