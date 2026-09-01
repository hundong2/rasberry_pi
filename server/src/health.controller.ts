import { Controller, Get } from '@nestjs/common';
import { CameraService } from './camera/camera.service.js';
import { OpenCvService } from './opencv/opencv.service.js';

@Controller('api')
export class HealthController {
  constructor(
    private readonly cameras: CameraService,
    private readonly openCv: OpenCvService,
  ) {}

  @Get('health')
  async health(): Promise<Record<string, unknown>> {
    const vision = await this.openCv.health();
    return {
      status: vision ? 'ok' : 'degraded',
      vision,
      uptimeSeconds: Math.floor(process.uptime()),
      timestamp: new Date().toISOString(),
    };
  }

  @Get('cameras')
  camerasList(): unknown[] {
    return this.cameras.list();
  }
}
