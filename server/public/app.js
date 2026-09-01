const frameElement = document.querySelector('#frame');
const emptyElement = document.querySelector('#empty');
const cameraSelect = document.querySelector('#camera');
const tokenInput = document.querySelector('#token');
const connection = document.querySelector('#connection');
let socket;
let objectUrl;
let frames = 0;
let fpsStartedAt = performance.now();

function connect() {
  socket?.disconnect();
  socket = io('/stream', {
    transports: ['websocket'],
    auth: { role: 'viewer', token: tokenInput.value },
  });
  socket.on('connect', () => setStatus('online', '연결됨'));
  socket.on('disconnect', () => setStatus('offline', '연결 끊김'));
  socket.on('connect_error', () => setStatus('offline', '인증/연결 실패'));
  socket.on('camera:list', (cameras) => cameras.forEach((camera) => upsertCamera(camera.cameraId)));
  socket.on('camera:status', ({ cameraId }) => upsertCamera(cameraId));
  socket.on('viewer:frame', renderFrame);
}

function renderFrame(message) {
  upsertCamera(message.cameraId);
  if (cameraSelect.value && cameraSelect.value !== message.cameraId) return;
  if (!cameraSelect.value) cameraSelect.value = message.cameraId;

  const bytes = message.frame instanceof ArrayBuffer ? message.frame : message.frame.buffer;
  if (objectUrl) URL.revokeObjectURL(objectUrl);
  objectUrl = URL.createObjectURL(new Blob([bytes], { type: 'image/jpeg' }));
  frameElement.src = objectUrl;
  frameElement.hidden = false;
  emptyElement.hidden = true;
  document.querySelector('#camera-id').textContent = message.cameraId;
  document.querySelector('#processing').textContent = `${message.processingMs.toFixed(1)} ms`;
  document.querySelector('#detections').textContent = message.detections.length;

  frames += 1;
  const elapsed = performance.now() - fpsStartedAt;
  if (elapsed >= 1_000) {
    document.querySelector('#fps').textContent = (frames * 1_000 / elapsed).toFixed(1);
    frames = 0;
    fpsStartedAt = performance.now();
  }
}

function upsertCamera(cameraId) {
  if ([...cameraSelect.options].some((option) => option.value === cameraId)) return;
  if (!cameraSelect.value) cameraSelect.innerHTML = '';
  cameraSelect.add(new Option(cameraId, cameraId));
}

function setStatus(className, text) {
  connection.className = `status ${className}`;
  connection.textContent = text;
}

document.querySelector('#connect').addEventListener('click', connect);
connect();
