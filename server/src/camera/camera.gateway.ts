import {
  ConnectedSocket,
  MessageBody,
  OnGatewayConnection,
  OnGatewayDisconnect,
  SubscribeMessage,
  WebSocketGateway,
  WebSocketServer,
} from '@nestjs/websockets';
import type { Server, Socket } from 'socket.io';
import { timingSafeEqual } from 'node:crypto';
import { CameraService } from './camera.service.js';
import type { ClientRole, IncomingFrame } from './camera.types.js';

interface AuthenticatedSocket extends Socket {
  data: {
    role?: ClientRole;
    cameraId?: string;
  };
}

@WebSocketGateway({
  namespace: '/stream',
  cors: { origin: true, credentials: true },
  maxHttpBufferSize: 2_113_536,
  transports: ['websocket'],
})
export class CameraGateway implements OnGatewayConnection, OnGatewayDisconnect {
  @WebSocketServer()
  private server!: Server;

  constructor(private readonly cameras: CameraService) {}

  handleConnection(client: AuthenticatedSocket): void {
    const role = client.handshake.auth.role as ClientRole | undefined;
    const token = String(client.handshake.auth.token ?? '');
    const cameraId = this.normalizeCameraId(client.handshake.auth.cameraId);

    if (role === 'camera') {
      if (!cameraId || !this.validToken(token, process.env.CAMERA_TOKEN)) {
        client.disconnect(true);
        return;
      }
      client.data.role = role;
      client.data.cameraId = cameraId;
      this.cameras.connect(cameraId);
      this.server.emit('camera:status', { cameraId, connected: true });
      return;
    }

    if (role === 'viewer' && this.validToken(token, process.env.VIEWER_TOKEN, true)) {
      client.data.role = role;
      client.emit('camera:list', this.cameras.list());
      return;
    }

    client.disconnect(true);
  }

  handleDisconnect(client: AuthenticatedSocket): void {
    if (client.data.role === 'camera' && client.data.cameraId) {
      this.cameras.disconnect(client.data.cameraId);
      this.server.emit('camera:status', {
        cameraId: client.data.cameraId,
        connected: false,
      });
    }
  }

  @SubscribeMessage('camera:frame')
  async onFrame(
    @ConnectedSocket() client: AuthenticatedSocket,
    @MessageBody() payload: IncomingFrame,
  ): Promise<{ accepted: boolean; reason?: string }> {
    if (client.data.role !== 'camera' || !client.data.cameraId) {
      return { accepted: false, reason: 'unauthorized' };
    }

    const frame = this.toBuffer(payload?.frame);
    const maxBytes = Number(process.env.MAX_FRAME_BYTES ?? 2_097_152);
    if (!frame || frame.length < 4 || frame.length > maxBytes || !this.isJpeg(frame)) {
      return { accepted: false, reason: 'invalid_jpeg' };
    }

    try {
      const result = await this.cameras.acceptFrame(
        client.data.cameraId,
        frame,
        Number(payload.timestamp) || Date.now(),
      );
      if (!result) return { accepted: false, reason: 'backpressure' };

      this.server.emit('viewer:frame', {
        cameraId: result.cameraId,
        timestamp: result.timestamp,
        processingMs: result.processingMs,
        detections: result.detections,
        frame: result.frame,
      });
      return { accepted: true };
    } catch (error) {
      console.error('Frame processing failed', error);
      return { accepted: false, reason: 'processing_failed' };
    }
  }

  private validToken(actual: string, configured?: string, allowEmpty = false): boolean {
    if (!configured) return allowEmpty;
    const actualBytes = Buffer.from(actual);
    const configuredBytes = Buffer.from(configured);
    return actualBytes.length === configuredBytes.length && timingSafeEqual(actualBytes, configuredBytes);
  }

  private normalizeCameraId(value: unknown): string | null {
    const cameraId = String(value ?? '').trim();
    return /^[a-zA-Z0-9_-]{1,64}$/.test(cameraId) ? cameraId : null;
  }

  private toBuffer(value: IncomingFrame['frame'] | undefined): Buffer | null {
    if (!value) return null;
    if (Buffer.isBuffer(value)) return value;
    if (value instanceof ArrayBuffer) return Buffer.from(value);
    if (value instanceof Uint8Array) return Buffer.from(value);
    return null;
  }

  private isJpeg(frame: Buffer): boolean {
    return frame[0] === 0xff && frame[1] === 0xd8 && frame.at(-2) === 0xff && frame.at(-1) === 0xd9;
  }
}
