import { Module } from '@nestjs/common';
import { CameraGateway } from './camera/camera.gateway.js';
import { CameraService } from './camera/camera.service.js';
import { HealthController } from './health.controller.js';
import { OpenCvService } from './opencv/opencv.service.js';

@Module({
  controllers: [HealthController],
  providers: [CameraGateway, CameraService, OpenCvService],
})
export class AppModule {}
