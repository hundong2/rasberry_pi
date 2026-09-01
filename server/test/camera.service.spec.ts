import { beforeEach, describe, expect, it, vi } from 'vitest';
import { CameraService } from '../src/camera/camera.service.js';
import type { OpenCvService } from '../src/opencv/opencv.service.js';

describe('CameraService', () => {
  beforeEach(() => {
    process.env.MAX_CAMERA_FPS = '1000';
  });

  it('tracks connected cameras', () => {
    const service = new CameraService({} as OpenCvService);
    service.connect('pi-1');
    expect(service.list()[0]).toMatchObject({ cameraId: 'pi-1', connected: true });
    service.disconnect('pi-1');
    expect(service.list()[0]?.connected).toBe(false);
  });

  it('drops a second frame while the first is processing', async () => {
    let resolveProcess: ((value: unknown) => void) | undefined;
    const process = vi.fn(() => new Promise((resolve) => { resolveProcess = resolve; }));
    const service = new CameraService({ process } as unknown as OpenCvService);
    service.connect('pi-1');
    const first = service.acceptFrame('pi-1', Buffer.from('frame'));
    const second = await service.acceptFrame('pi-1', Buffer.from('frame'));
    expect(second).toBeNull();
    expect(service.list()[0]?.droppedFrames).toBe(1);
    resolveProcess?.({ cameraId: 'pi-1', frame: Buffer.alloc(1), detections: [], timestamp: 1, processingMs: 1 });
    await first;
  });
});
