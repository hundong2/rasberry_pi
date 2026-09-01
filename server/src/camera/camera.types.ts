// `type` alias는 값이 아니라 컴파일 때만 사용하는 타입 이름을 만든다.
// 문자열 literal union은 role이 두 문자열 중 하나만 되게 제한한다.
export type ClientRole = 'camera' | 'viewer';

// `interface`는 객체가 가져야 할 property 이름과 타입을 정의한다.
// JavaScript 실행 결과에는 interface 코드가 남지 않으며 TypeScript 검사용이다.
export interface IncomingFrame {
  // 송신 payload가 표준화되도록 camera ID를 문자열로 선언한다.
  cameraId: string;

  // `?`는 property가 없어도 된다는 뜻이다. 없으면 서버가 현재 시각을 사용한다.
  timestamp?: number;

  // `|`는 union type이다. Socket.IO가 전달할 수 있는 세 binary 표현을 모두 허용한다.
  frame: Buffer | Uint8Array | ArrayBuffer;
}

// vision service가 찾은 한 개 영역의 공통 형식이다.
export interface Detection {
  // 현재는 `motion`, 나중에는 `person`, `cell_phone` 같은 label이 들어간다.
  type: string;

  // confidence는 관례상 0~1 범위지만, interface 자체는 범위를 런타임 검증하지 않는다.
  confidence: number;

  // x/y는 원본 이미지에서 bounding box 왼쪽 위 좌표(pixel)다.
  x: number;
  y: number;

  // width/height는 bounding box 크기(pixel)다.
  width: number;
  height: number;
}

// FastAPI vision 처리 후 NestJS가 받는 한 frame의 결과다.
export interface ProcessedFrame {
  cameraId: string;
  // Unix epoch millisecond를 number로 전달한다.
  timestamp: number;
  // vision service 내부 처리 시간 단위는 millisecond다.
  processingMs: number;
  // `Detection[]`는 Detection 객체가 0개 이상 들어 있는 배열이다.
  detections: Detection[];
  // `Buffer`는 Node.js가 binary byte sequence를 표현하는 class다.
  frame: Buffer;
}

// `/api/cameras`와 viewer 초기 목록에 사용되는 camera별 누적 상태다.
export interface CameraState {
  cameraId: string;
  connected: boolean;

  // `number | null`: 아직 frame이 없으면 null, 있으면 마지막 처리 완료 시각이다.
  lastFrameAt: number | null;
  receivedFrames: number;
  droppedFrames: number;
}
