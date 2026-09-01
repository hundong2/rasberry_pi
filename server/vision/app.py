import base64
import time
from typing import Annotated

import cv2
import numpy as np
from fastapi import FastAPI, File, Form, HTTPException, UploadFile

app = FastAPI(title="Camera OpenCV Processor", version="0.1.0")
previous_frames: dict[str, np.ndarray] = {}


@app.get("/health")
def health() -> dict[str, str]:
    return {"status": "ok", "opencv": cv2.__version__}


@app.post("/process")
async def process(
    frame: Annotated[UploadFile, File()],
    camera_id: Annotated[str, Form()],
    timestamp: Annotated[int, Form()],
    mode: Annotated[str, Form()] = "motion",
    jpeg_quality: Annotated[int, Form()] = 80,
) -> dict:
    started = time.perf_counter()
    if mode not in {"motion", "edges", "none"}:
        raise HTTPException(400, "mode must be motion, edges, or none")

    raw = await frame.read()
    image = cv2.imdecode(np.frombuffer(raw, dtype=np.uint8), cv2.IMREAD_COLOR)
    if image is None:
        raise HTTPException(400, "invalid JPEG frame")

    detections: list[dict] = []
    if mode == "motion":
        detections = detect_motion(camera_id, image)
        draw_detections(image, detections)
    elif mode == "edges":
        gray = cv2.cvtColor(image, cv2.COLOR_BGR2GRAY)
        edges = cv2.Canny(gray, 80, 160)
        image = cv2.cvtColor(edges, cv2.COLOR_GRAY2BGR)

    quality = min(95, max(30, jpeg_quality))
    encoded, output = cv2.imencode(".jpg", image, [cv2.IMWRITE_JPEG_QUALITY, quality])
    if not encoded:
        raise HTTPException(500, "failed to encode processed frame")

    return {
        "cameraId": camera_id,
        "timestamp": timestamp,
        "processingMs": round((time.perf_counter() - started) * 1000, 2),
        "detections": detections,
        "imageBase64": base64.b64encode(output).decode("ascii"),
    }


def detect_motion(camera_id: str, image: np.ndarray) -> list[dict]:
    gray = cv2.cvtColor(image, cv2.COLOR_BGR2GRAY)
    gray = cv2.GaussianBlur(gray, (21, 21), 0)
    previous = previous_frames.get(camera_id)
    previous_frames[camera_id] = gray
    if previous is None or previous.shape != gray.shape:
        return []

    delta = cv2.absdiff(previous, gray)
    threshold = cv2.threshold(delta, 25, 255, cv2.THRESH_BINARY)[1]
    threshold = cv2.dilate(threshold, None, iterations=2)
    contours, _ = cv2.findContours(threshold, cv2.RETR_EXTERNAL, cv2.CHAIN_APPROX_SIMPLE)

    image_area = image.shape[0] * image.shape[1]
    minimum_area = max(500, image_area * 0.002)
    detections = []
    for contour in contours:
        area = cv2.contourArea(contour)
        if area < minimum_area:
            continue
        x, y, width, height = cv2.boundingRect(contour)
        detections.append({
            "type": "motion",
            "confidence": round(min(1.0, area / (image_area * 0.1)), 3),
            "x": int(x), "y": int(y), "width": int(width), "height": int(height),
        })
    return sorted(detections, key=lambda item: item["width"] * item["height"], reverse=True)[:20]


def draw_detections(image: np.ndarray, detections: list[dict]) -> None:
    for detection in detections:
        start = (detection["x"], detection["y"])
        end = (detection["x"] + detection["width"], detection["y"] + detection["height"])
        cv2.rectangle(image, start, end, (0, 255, 0), 2)
        cv2.putText(image, "motion", (start[0], max(20, start[1] - 6)),
                    cv2.FONT_HERSHEY_SIMPLEX, 0.55, (0, 255, 0), 2)
