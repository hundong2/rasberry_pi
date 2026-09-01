// Injectable은 이 class를 NestJS provider로 등록 가능하게 하는 decorator다.
import { Injectable } from '@nestjs/common';

// ProcessedFrame은 type으로만 쓰이므로 JavaScript import를 만들지 않는다.
import type { ProcessedFrame } from '../camera/camera.types.js';

// 외부 FastAPI JSON 응답의 모양을 TypeScript에 알려준다.
// interface는 runtime JSON을 자동 검증하지 않으므로 신뢰 경계에서는 schema 검증이 후속 과제다.
interface VisionResponse {
  cameraId: string;
  timestamp: number;
  processingMs: number;

  // indexed access type은 ProcessedFrame의 detections property 타입을 중복 선언 없이 재사용한다.
  detections: ProcessedFrame['detections'];

  // JSON은 raw Buffer를 직접 표현하지 못해 현재 FastAPI가 JPEG를 Base64 문자열로 반환한다.
  imageBase64: string;
}

@Injectable()
export class OpenCvService {
  // 환경 변수가 없으면 local FastAPI 8000을 사용한다.
  // replace(/\/$/, '')는 URL 맨 끝 slash 하나를 빈 문자열로 바꿔 `//health`를 방지한다.
  private readonly baseUrl = (process.env.OPENCV_URL ?? 'http://localhost:8000').replace(
    /\/$/,
    '',
  );

  /** vision service health를 확인하고 네트워크 오류를 false로 변환한다. */
  async health(): Promise<boolean> {
    try {
      // 전역 fetch()는 HTTP 요청 Promise를 반환하고 await는 Response 객체를 얻는다.
      const response = await fetch(`${this.baseUrl}/health`, {
        // AbortSignal.timeout()은 지정 시간이 지나면 fetch를 취소하는 signal을 반환한다.
        signal: AbortSignal.timeout(1_500),
      });

      // Response.ok는 HTTP status 200~299이면 true인 boolean property다.
      return response.ok;
    } catch {
      // timeout, DNS, 연결 거부, JSON과 무관한 transport 예외를 health=false로 단순화한다.
      return false;
    }
  }

  /** JPEG와 metadata를 multipart/form-data로 FastAPI에 보내 처리 결과를 반환한다. */
  async process(cameraId: string, frame: Buffer, timestamp: number): Promise<ProcessedFrame> {
    // FormData는 browser의 HTML form과 같은 multipart HTTP body를 구성한다.
    const form = new FormData();

    // append(name, value)는 field를 추가하고 반환값은 void다.
    form.append('camera_id', cameraId);

    // multipart text field는 문자열이어야 하므로 number timestamp를 String()으로 변환한다.
    form.append('timestamp', String(timestamp));

    // vision mode/JPEG quality도 환경 변수 또는 안전한 기본 문자열을 사용한다.
    form.append('mode', process.env.VISION_MODE ?? 'motion');
    form.append('jpeg_quality', process.env.JPEG_QUALITY ?? '80');

    // Uint8Array.from(Buffer)는 byte 배열을 복사한다. Blob은 MIME type이 있는 binary 객체다.
    // 세 번째 인자 `frame.jpg`는 multipart upload filename이다.
    form.append(
      'frame',
      new Blob([Uint8Array.from(frame)], { type: 'image/jpeg' }),
      'frame.jpg',
    );

    // POST `/process`를 호출한다. FormData를 body로 주면 fetch가 boundary Content-Type을 만든다.
    const response = await fetch(`${this.baseUrl}/process`, {
      method: 'POST',
      body: form,
      // vision이 5초를 넘기면 이 frame 요청을 취소해 무한 대기를 막는다.
      signal: AbortSignal.timeout(5_000),
    });

    // HTTP 4xx/5xx는 fetch 자체가 예외를 던지지 않으므로 ok를 직접 검사해야 한다.
    if (!response.ok) {
      // Error 객체를 throw하면 CameraGateway의 catch까지 Promise rejection이 전파된다.
      throw new Error(`OpenCV service returned ${response.status}`);
    }

    // response.json()은 body를 parsing한 값을 Promise로 반환한다.
    // `as VisionResponse`는 compile-time assertion일 뿐 runtime validation은 아니다.
    const result = (await response.json()) as VisionResponse;

    // 외부 응답을 프로젝트 내부 ProcessedFrame 형식으로 명시적으로 mapping한다.
    return {
      cameraId: result.cameraId,
      timestamp: result.timestamp,
      processingMs: result.processingMs,
      detections: result.detections,

      // Buffer.from(base64, 'base64')는 Base64 text를 원래 JPEG bytes로 decode한다.
      frame: Buffer.from(result.imageBase64, 'base64'),
    };
  }
}
