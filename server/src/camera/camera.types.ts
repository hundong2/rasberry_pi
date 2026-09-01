export type ClientRole = 'camera' | 'viewer';

export interface IncomingFrame {
  cameraId: string;
  timestamp?: number;
  frame: Buffer | Uint8Array | ArrayBuffer;
}

export interface Detection {
  type: string;
  confidence: number;
  x: number;
  y: number;
  width: number;
  height: number;
}

export interface ProcessedFrame {
  cameraId: string;
  timestamp: number;
  processingMs: number;
  detections: Detection[];
  frame: Buffer;
}

export interface CameraState {
  cameraId: string;
  connected: boolean;
  lastFrameAt: number | null;
  receivedFrames: number;
  droppedFrames: number;
}
