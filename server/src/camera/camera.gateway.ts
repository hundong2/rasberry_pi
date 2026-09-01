// 아래 이름들은 NestJS WebSocket 기능을 class/method/parameter에 연결하는 decorator와
// gateway lifecycle interface다. 중괄호 import는 package의 named export를 가져온다.
import {
  ConnectedSocket,
  MessageBody,
  OnGatewayConnection,
  OnGatewayDisconnect,
  SubscribeMessage,
  WebSocketGateway,
  WebSocketServer,
} from '@nestjs/websockets';

// `import type`은 컴파일 검사용 타입만 가져오며 빌드된 JavaScript import에는 남지 않는다.
// Server는 Socket.IO 서버 전체, Socket은 client 연결 한 개를 나타낸다.
import type { Server, Socket } from 'socket.io';

// Node.js 내장 crypto 함수다. 같은 길이의 두 Buffer를 비교할 때 timing 차이를 줄인다.
import { timingSafeEqual } from 'node:crypto';

// CameraGateway는 상태/처리를 직접 구현하지 않고 CameraService에 위임한다.
import { CameraService } from './camera.service.js';
import type { ClientRole, IncomingFrame } from './camera.types.js';

// `extends Socket`은 기존 Socket 타입의 모든 property에 인증 후 저장할 data 타입을 더한다.
// interface는 실행 코드가 아니라 TypeScript가 안전성을 검사하기 위한 설계도다.
interface AuthenticatedSocket extends Socket {
  data: {
    // `?`는 연결 직후 아직 인증되지 않아 값이 없을 수 있다는 뜻이다.
    role?: ClientRole;
    cameraId?: string;
  };
}

// 바로 아래 class를 Socket.IO gateway로 등록하면서 연결 옵션을 전달한다.
@WebSocketGateway({
  // namespace는 기본 `/`와 분리된 논리적인 Socket.IO endpoint다.
  namespace: '/stream',

  // browser CORS origin을 허용하고 credential 포함 연결을 허용한다.
  cors: { origin: true, credentials: true },

  // 한 Socket.IO message의 최대 byte다. 숫자 `_`는 읽기용 구분자로 값에는 영향이 없다.
  maxHttpBufferSize: 2_113_536,

  // polling fallback 없이 WebSocket transport만 허용해 binary stream 경로를 단순화한다.
  transports: ['websocket'],
})
// `implements`는 두 lifecycle interface가 요구하는 method를 class가 제공하는지 검사한다.
export class CameraGateway implements OnGatewayConnection, OnGatewayDisconnect {
  // NestJS가 생성한 namespace Server 객체를 이 field에 주입한다.
  @WebSocketServer()
  // `private`는 class 내부 전용이다. `!`는 NestJS가 나중에 값을 넣으므로 초기화 오류를
  // 내지 말라는 definite-assignment assertion이다.
  private server!: Server;

  // NestJS DI가 CameraService 인스턴스를 넣는다. parameter property 문법이라 별도 field
  // 선언과 `this.cameras = cameras` 대입을 자동으로 만든다.
  constructor(private readonly cameras: CameraService) {}

  /**
   * 새 Socket.IO 연결을 camera/viewer로 인증한다.
   * @param client 방금 연결된 socket 한 개.
   * @returns `void`: 연결을 유지하거나 disconnect할 뿐 호출자에게 값을 반환하지 않는다.
   */
  handleConnection(client: AuthenticatedSocket): void {
    // handshake.auth는 client가 연결할 때 보낸 일반 객체다.
    // `as`는 런타임 변환이 아니라 개발자가 기대 타입을 compiler에 알려주는 assertion이다.
    const role = client.handshake.auth.role as ClientRole | undefined;

    // `?? ''`는 token이 null/undefined면 빈 문자열을 사용한다.
    // String()은 숫자 등 다른 입력도 문자열로 변환해 비교 함수 입력 타입을 통일한다.
    const token = String(client.handshake.auth.token ?? '');

    // 외부 입력 cameraId를 바로 신뢰하지 않고 허용 문자/길이를 검사한다.
    const cameraId = this.normalizeCameraId(client.handshake.auth.cameraId);

    // strict equality `===`는 타입 변환 없이 role이 정확히 `camera`인지 비교한다.
    if (role === 'camera') {
      // `||`는 둘 중 하나라도 참이면 차단한다. `!cameraId`는 null 또는 빈 값을 뜻한다.
      if (!cameraId || !this.validToken(token, process.env.CAMERA_TOKEN)) {
        // disconnect(true)는 namespace뿐 아니라 underlying connection도 강제로 닫는다.
        client.disconnect(true);
        // `return`으로 아래 인증 성공 코드를 실행하지 않고 함수를 끝낸다.
        return;
      }

      // 인증 이후 socket.data에 값을 저장하면 후속 frame event에서 다시 auth를 파싱하지 않는다.
      client.data.role = role;
      client.data.cameraId = cameraId;

      // connect()는 CameraState를 connected=true로 만들고 기존 누적 통계는 유지한다.
      this.cameras.connect(cameraId);

      // server.emit()은 namespace의 모든 연결에 event를 broadcast하며 Server를 반환하지만
      // 여기서는 반환값이 필요 없다.
      this.server.emit('camera:status', { cameraId, connected: true });
      return;
    }

    // `&&`는 role이 viewer이고 token도 유효할 때만 오른쪽 block을 실행한다.
    // 세 번째 인자 true는 서버 VIEWER_TOKEN이 비었을 때 공개 viewer를 허용한다.
    if (role === 'viewer' && this.validToken(token, process.env.VIEWER_TOKEN, true)) {
      client.data.role = role;

      // client.emit()은 이 viewer 한 명에게만 현재 camera 목록을 전송한다.
      client.emit('camera:list', this.cameras.list());
      return;
    }

    // camera도 정상 viewer도 아닌 연결은 권한이 없으므로 닫는다.
    client.disconnect(true);
  }

  /** 연결 종료 시 camera 상태를 갱신해 viewer에 알린다. */
  handleDisconnect(client: AuthenticatedSocket): void {
    // viewer 종료에는 camera 상태를 바꿀 필요가 없다. 두 조건이 모두 참일 때만 실행한다.
    if (client.data.role === 'camera' && client.data.cameraId) {
      // disconnect()는 해당 camera state의 connected만 false로 바꾼다.
      this.cameras.disconnect(client.data.cameraId);

      // 모든 viewer가 camera offline 상태를 즉시 UI에 반영할 수 있게 broadcast한다.
      this.server.emit('camera:status', {
        cameraId: client.data.cameraId,
        connected: false,
      });
    }
  }

  // camera가 `camera:frame` event를 보내면 바로 아래 method를 호출하도록 등록한다.
  @SubscribeMessage('camera:frame')
  async onFrame(
    // parameter decorator는 현재 event를 보낸 Socket을 이 parameter에 주입한다.
    @ConnectedSocket() client: AuthenticatedSocket,
    // event의 payload 객체를 이 parameter에 주입한다.
    @MessageBody() payload: IncomingFrame,
    // Promise 안 객체의 `reason?`은 실패 때만 reason 문자열이 있을 수 있다는 뜻이다.
  ): Promise<{ accepted: boolean; reason?: string }> {
    // 연결 때 인증해 둔 role/cameraId를 확인한다. `!==`는 strict not-equal 비교다.
    if (client.data.role !== 'camera' || !client.data.cameraId) {
      // 이 객체가 Socket.IO ACK의 첫 번째 인자로 client에 돌아간다.
      return { accepted: false, reason: 'unauthorized' };
    }

    // `payload?.frame` optional chaining은 payload가 null/undefined여도 예외 대신 undefined다.
    // toBuffer() 반환값은 지원 binary면 Buffer, 아니면 null이다.
    const frame = this.toBuffer(payload?.frame);

    // 환경 변수가 없을 때 2 MiB를 사용하고 Number()로 문자열을 숫자로 변환한다.
    const maxBytes = Number(process.env.MAX_FRAME_BYTES ?? 2_097_152);

    // frame 존재, 최소 marker 길이, 최대 크기, JPEG 시작/끝 marker를 한 번에 검사한다.
    if (!frame || frame.length < 4 || frame.length > maxBytes || !this.isJpeg(frame)) {
      return { accepted: false, reason: 'invalid_jpeg' };
    }

    // try/catch는 vision/network 예외가 gateway 전체를 중단시키지 않게 frame 단위로 처리한다.
    try {
      // acceptFrame()은 비동기 vision 결과 또는 backpressure일 때 null을 Promise로 반환한다.
      const result = await this.cameras.acceptFrame(
        client.data.cameraId,
        frame,
        // Number() 결과가 0/NaN이면 `||` 오른쪽의 현재 epoch millisecond를 사용한다.
        Number(payload.timestamp) || Date.now(),
      );

      // null은 오류가 아니라 서버가 이미 처리 중이거나 FPS 상한을 넘은 정상 drop이다.
      if (!result) return { accepted: false, reason: 'backpressure' };

      // 처리 완료 frame과 metadata를 현재 namespace의 모든 viewer에게 broadcast한다.
      this.server.emit('viewer:frame', {
        cameraId: result.cameraId,
        timestamp: result.timestamp,
        processingMs: result.processingMs,
        detections: result.detections,
        frame: result.frame,
      });

      // camera client의 ACK callback은 accepted=true를 받아 다음 frame을 진행한다.
      return { accepted: true };
    } catch (error) {
      // catch 변수는 어떤 값도 될 수 있다. console.error는 메시지와 stack을 stderr에 남긴다.
      console.error('Frame processing failed', error);
      return { accepted: false, reason: 'processing_failed' };
    }
  }

  /** 실제 token과 server 설정 token을 timing-safe 방식으로 비교한다. */
  private validToken(actual: string, configured?: string, allowEmpty = false): boolean {
    // configured가 undefined/빈 문자열이면 viewer만 allowEmpty=true일 때 허용한다.
    if (!configured) return allowEmpty;

    // Buffer.from(string)은 UTF-8 문자열을 byte Buffer로 변환한다.
    const actualBytes = Buffer.from(actual);
    const configuredBytes = Buffer.from(configured);

    // timingSafeEqual()은 Buffer 길이가 다르면 예외를 던지므로 길이를 먼저 비교한다.
    // `&&`는 길이가 다르면 오른쪽 함수를 호출하지 않는 short-circuit 연산자다.
    return (
      actualBytes.length === configuredBytes.length &&
      timingSafeEqual(actualBytes, configuredBytes)
    );
  }

  /** 외부 입력을 안전한 camera ID 문자열 또는 null로 정규화한다. */
  private normalizeCameraId(value: unknown): string | null {
    // unknown을 String()으로 변환한 뒤 trim()으로 앞뒤 공백을 제거한 새 문자열을 얻는다.
    const cameraId = String(value ?? '').trim();

    // RegExp.test()는 전체 문자열이 허용 문자 1~64개인지 boolean으로 반환한다.
    // 삼항 연산자는 유효하면 cameraId, 아니면 null을 반환한다.
    return /^[a-zA-Z0-9_-]{1,64}$/.test(cameraId) ? cameraId : null;
  }

  /** Socket.IO가 전달한 여러 binary 표현을 Node Buffer 하나로 통일한다. */
  private toBuffer(value: IncomingFrame['frame'] | undefined): Buffer | null {
    // indexed access type `IncomingFrame['frame']`은 interface의 frame property 타입을 재사용한다.
    if (!value) return null;

    // Buffer.isBuffer()는 값이 이미 Node Buffer인지 boolean으로 알려준다. 복사가 필요 없다.
    if (Buffer.isBuffer(value)) return value;

    // `instanceof`는 객체가 해당 constructor로 만들어졌는지 런타임에 검사한다.
    if (value instanceof ArrayBuffer) return Buffer.from(value);
    if (value instanceof Uint8Array) return Buffer.from(value);

    // TypeScript union 밖의 잘못된 런타임 입력도 들어올 수 있으므로 null로 거부한다.
    return null;
  }

  /** JPEG SOI/EOI magic byte를 검사한다. 완전한 JPEG decode 검사는 vision service가 한다. */
  private isJpeg(frame: Buffer): boolean {
    // JPEG 시작은 FF D8, 끝은 FF D9다. at(-1/-2)는 배열 뒤에서 원소를 읽는다.
    return (
      frame[0] === 0xff &&
      frame[1] === 0xd8 &&
      frame.at(-2) === 0xff &&
      frame.at(-1) === 0xd9
    );
  }
}
