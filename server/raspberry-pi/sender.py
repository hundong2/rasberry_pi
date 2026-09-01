#!/usr/bin/env python3
import argparse
import logging
import signal
import time

import cv2
import socketio
from picamera2 import Picamera2


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Send Raspberry Pi camera JPEG frames to the viewer server")
    parser.add_argument("--server", required=True, help="Example: http://192.168.0.10:3000")
    parser.add_argument("--token", required=True)
    parser.add_argument("--camera-id", default="raspberry-pi-1")
    parser.add_argument("--width", type=int, default=1280)
    parser.add_argument("--height", type=int, default=720)
    parser.add_argument("--fps", type=float, default=10)
    parser.add_argument("--quality", type=int, default=80)
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
    client = socketio.Client(reconnection=True, logger=False)
    client.connect(
        args.server,
        namespaces=["/stream"],
        transports=["websocket"],
        auth={"role": "camera", "cameraId": args.camera_id, "token": args.token},
    )

    camera = Picamera2()
    config = camera.create_video_configuration(
        main={"size": (args.width, args.height), "format": "RGB888"},
        controls={"FrameRate": args.fps},
    )
    camera.configure(config)
    camera.start()
    running = True

    def stop(*_: object) -> None:
        nonlocal running
        running = False

    signal.signal(signal.SIGINT, stop)
    signal.signal(signal.SIGTERM, stop)
    interval = 1.0 / max(1.0, args.fps)
    logging.info("Streaming %s to %s", args.camera_id, args.server)

    try:
        while running:
            started = time.monotonic()
            rgb = camera.capture_array()
            bgr = cv2.cvtColor(rgb, cv2.COLOR_RGB2BGR)
            ok, jpeg = cv2.imencode(".jpg", bgr, [cv2.IMWRITE_JPEG_QUALITY, args.quality])
            if ok and client.connected:
                client.emit(
                    "camera:frame",
                    {"timestamp": int(time.time() * 1000), "frame": jpeg.tobytes()},
                    namespace="/stream",
                )
            time.sleep(max(0, interval - (time.monotonic() - started)))
    finally:
        camera.stop()
        client.disconnect()


if __name__ == "__main__":
    main()
