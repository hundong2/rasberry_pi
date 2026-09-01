import { Injectable } from '@nestjs/common';
import type { ProcessedFrame } from '../camera/camera.types.js';

interface VisionResponse {
  cameraId: string;
  timestamp: number;
  processingMs: number;
  detections: ProcessedFrame['detections'];
  imageBase64: string;
}

@Injectable()
export class OpenCvService {
  private readonly baseUrl = (process.env.OPENCV_URL ?? 'http://localhost:8000').replace(/\/$/, '');

  async health(): Promise<boolean> {
    try {
      const response = await fetch(`${this.baseUrl}/health`, {
        signal: AbortSignal.timeout(1_500),
      });
      return response.ok;
    } catch {
      return false;
    }
  }

  async process(cameraId: string, frame: Buffer, timestamp: number): Promise<ProcessedFrame> {
    const form = new FormData();
    form.append('camera_id', cameraId);
    form.append('timestamp', String(timestamp));
    form.append('mode', process.env.VISION_MODE ?? 'motion');
    form.append('jpeg_quality', process.env.JPEG_QUALITY ?? '80');
    form.append('frame', new Blob([Uint8Array.from(frame)], { type: 'image/jpeg' }), 'frame.jpg');

    const response = await fetch(`${this.baseUrl}/process`, {
      method: 'POST',
      body: form,
      signal: AbortSignal.timeout(5_000),
    });
    if (!response.ok) {
      throw new Error(`OpenCV service returned ${response.status}`);
    }

    const result = (await response.json()) as VisionResponse;
    return {
      cameraId: result.cameraId,
      timestamp: result.timestamp,
      processingMs: result.processingMs,
      detections: result.detections,
      frame: Buffer.from(result.imageBase64, 'base64'),
    };
  }
}
