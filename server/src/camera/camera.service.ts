import { Injectable } from '@nestjs/common';
import { OpenCvService } from '../opencv/opencv.service.js';
import type { CameraState, ProcessedFrame } from './camera.types.js';

@Injectable()
export class CameraService {
  private readonly states = new Map<string, CameraState>();
  private readonly processing = new Set<string>();
  private readonly lastAcceptedAt = new Map<string, number>();

  constructor(private readonly openCv: OpenCvService) {}

  connect(cameraId: string): void {
    const existing = this.states.get(cameraId);
    this.states.set(cameraId, {
      cameraId,
      connected: true,
      lastFrameAt: existing?.lastFrameAt ?? null,
      receivedFrames: existing?.receivedFrames ?? 0,
      droppedFrames: existing?.droppedFrames ?? 0,
    });
  }

  disconnect(cameraId: string): void {
    const state = this.states.get(cameraId);
    if (state) state.connected = false;
  }

  list(): CameraState[] {
    return [...this.states.values()].map((state) => ({ ...state }));
  }

  async acceptFrame(
    cameraId: string,
    frame: Buffer,
    timestamp = Date.now(),
  ): Promise<ProcessedFrame | null> {
    const state = this.states.get(cameraId);
    if (!state) this.connect(cameraId);
    const current = this.states.get(cameraId)!;
    current.receivedFrames += 1;

    const maxFps = Math.max(1, Number(process.env.MAX_CAMERA_FPS ?? 12));
    const minimumInterval = 1_000 / maxFps;
    const now = Date.now();
    const tooFast = now - (this.lastAcceptedAt.get(cameraId) ?? 0) < minimumInterval;

    if (this.processing.has(cameraId) || tooFast) {
      current.droppedFrames += 1;
      return null;
    }

    this.processing.add(cameraId);
    this.lastAcceptedAt.set(cameraId, now);
    try {
      const processed = await this.openCv.process(cameraId, frame, timestamp);
      current.lastFrameAt = Date.now();
      return processed;
    } finally {
      this.processing.delete(cameraId);
    }
  }
}
