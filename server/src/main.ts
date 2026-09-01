import 'dotenv/config';
import { NestFactory } from '@nestjs/core';
import { NestExpressApplication } from '@nestjs/platform-express';
import { join } from 'node:path';
import { AppModule } from './app.module.js';

async function bootstrap(): Promise<void> {
  const app = await NestFactory.create<NestExpressApplication>(AppModule, {
    bodyParser: false,
  });

  app.enableShutdownHooks();
  app.enableCors({ origin: true, credentials: true });
  app.useStaticAssets(join(process.cwd(), 'public'));

  const port = Number(process.env.PORT ?? 3000);
  await app.listen(port, '0.0.0.0');
  console.info(`Camera viewer listening on http://0.0.0.0:${port}`);
}

void bootstrap();
