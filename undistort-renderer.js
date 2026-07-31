const canvas = document.querySelector("#effect");
const hud = document.querySelector("#hud");

const gl = canvas.getContext("webgl", {
  alpha: true,
  antialias: false,
  depth: false,
  stencil: false,
  premultipliedAlpha: true,
  preserveDrawingBuffer: false,
  powerPreference: "high-performance",
});

if (!gl) {
  throw new Error("当前显卡或驱动不支持 WebGL。");
}

const vertexShaderSource = `
  attribute vec2 a_position;
  varying vec2 v_uv;

  void main() {
    v_uv = a_position * 0.5 + 0.5;
    gl_Position = vec4(a_position, 0.0, 1.0);
  }
`;

const fragmentShaderSource = `
  precision highp float;

  uniform sampler2D u_screen;
  uniform vec2 u_resolution;
  uniform vec2 u_center;
  uniform float u_k1;
  uniform float u_k2;
  uniform float u_zoom;
  uniform float u_grid;
  uniform float u_enabled;

  varying vec2 v_uv;

  void main() {
    vec2 aspect = vec2(u_resolution.x / u_resolution.y, 1.0);
    vec2 p = (v_uv - u_center) * aspect;
    float r2 = dot(p, p);

    // Brown-style radial model. k1 controls the main corner curvature and k2
    // controls the higher-order bend near the extreme edges.
    float radial = 1.0 + u_k1 * r2 + u_k2 * r2 * r2;
    vec2 sourceP = p * radial / u_zoom;
    vec2 sourceUv = u_center + sourceP / aspect;

    float inside =
      step(0.0, sourceUv.x) * step(sourceUv.x, 1.0) *
      step(0.0, sourceUv.y) * step(sourceUv.y, 1.0);

    vec3 desktop = texture2D(u_screen, clamp(sourceUv, 0.0, 1.0)).rgb;
    vec3 color = mix(vec3(0.004, 0.008, 0.014), desktop, inside);

    // The calibration grid is sampled in source coordinates, so it receives
    // exactly the same pre-warp as the desktop image.
    vec2 gridCell = fract(sourceUv * vec2(16.0, 9.0));
    vec2 gridDistance = min(gridCell, 1.0 - gridCell);
    float gridLine = 1.0 - smoothstep(0.0, 0.018, min(gridDistance.x, gridDistance.y));
    float centerX = 1.0 - smoothstep(0.0, 0.0025, abs(sourceUv.x - 0.5));
    float centerY = 1.0 - smoothstep(0.0, 0.0025, abs(sourceUv.y - 0.5));
    vec3 gridColor = mix(vec3(0.10, 0.72, 1.0), vec3(1.0, 0.34, 0.12), max(centerX, centerY));
    float calibrationLine = max(gridLine * 0.68, max(centerX, centerY));
    color = mix(color, gridColor, calibrationLine * u_grid * inside);

    // Premultiplied alpha. In safety-bypass mode alpha is zero, revealing the
    // untouched real desktop underneath the overlay immediately.
    gl_FragColor = vec4(color * u_enabled, u_enabled);
  }
`;

function compileShader(type, source) {
  const shader = gl.createShader(type);
  gl.shaderSource(shader, source);
  gl.compileShader(shader);

  if (!gl.getShaderParameter(shader, gl.COMPILE_STATUS)) {
    const message = gl.getShaderInfoLog(shader);
    gl.deleteShader(shader);
    throw new Error(`Shader compilation failed: ${message}`);
  }
  return shader;
}

function createProgram() {
  const program = gl.createProgram();
  const vertexShader = compileShader(gl.VERTEX_SHADER, vertexShaderSource);
  const fragmentShader = compileShader(gl.FRAGMENT_SHADER, fragmentShaderSource);
  gl.attachShader(program, vertexShader);
  gl.attachShader(program, fragmentShader);
  gl.linkProgram(program);
  gl.deleteShader(vertexShader);
  gl.deleteShader(fragmentShader);

  if (!gl.getProgramParameter(program, gl.LINK_STATUS)) {
    throw new Error(`Shader link failed: ${gl.getProgramInfoLog(program)}`);
  }
  return program;
}

const program = createProgram();
const locations = {
  position: gl.getAttribLocation(program, "a_position"),
  screen: gl.getUniformLocation(program, "u_screen"),
  resolution: gl.getUniformLocation(program, "u_resolution"),
  center: gl.getUniformLocation(program, "u_center"),
  k1: gl.getUniformLocation(program, "u_k1"),
  k2: gl.getUniformLocation(program, "u_k2"),
  zoom: gl.getUniformLocation(program, "u_zoom"),
  grid: gl.getUniformLocation(program, "u_grid"),
  enabled: gl.getUniformLocation(program, "u_enabled"),
};

const geometry = gl.createBuffer();
gl.bindBuffer(gl.ARRAY_BUFFER, geometry);
gl.bufferData(
  gl.ARRAY_BUFFER,
  new Float32Array([-1, -1, 3, -1, -1, 3]),
  gl.STATIC_DRAW,
);

const screenTexture = gl.createTexture();
gl.bindTexture(gl.TEXTURE_2D, screenTexture);
gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
gl.pixelStorei(gl.UNPACK_FLIP_Y_WEBGL, true);

gl.useProgram(program);
gl.bindBuffer(gl.ARRAY_BUFFER, geometry);
gl.enableVertexAttribArray(locations.position);
gl.vertexAttribPointer(locations.position, 2, gl.FLOAT, false, 0, 0);
gl.uniform1i(locations.screen, 0);
gl.enable(gl.BLEND);
gl.blendFunc(gl.ONE, gl.ONE_MINUS_SRC_ALPHA);

const defaults = {
  k1: 0.0,
  k2: 0.0,
  zoom: 1.0,
  center: [0.5, 0.5],
  grid: true,
};

function loadParameters() {
  try {
    const saved = JSON.parse(localStorage.getItem("desktop-undistort-parameters"));
    if (!saved || typeof saved !== "object") return structuredClone(defaults);

    return {
      k1: Number.isFinite(saved.k1) ? Math.max(-2, Math.min(2, saved.k1)) : defaults.k1,
      k2: Number.isFinite(saved.k2) ? Math.max(-3, Math.min(3, saved.k2)) : defaults.k2,
      zoom: Number.isFinite(saved.zoom)
        ? Math.max(0.65, Math.min(1.6, saved.zoom))
        : defaults.zoom,
      center:
        Array.isArray(saved.center) && saved.center.length === 2
          ? [
              Math.max(0.35, Math.min(0.65, Number(saved.center[0]) || 0.5)),
              Math.max(0.35, Math.min(0.65, Number(saved.center[1]) || 0.5)),
            ]
          : [...defaults.center],
      grid: typeof saved.grid === "boolean" ? saved.grid : defaults.grid,
    };
  } catch {
    return structuredClone(defaults);
  }
}

const state = {
  ...loadParameters(),
  enabled: false,
};

function reportState() {
  window.desktopBlackHole.reportState({
    enabled: state.enabled,
    grid: state.grid,
    k1: state.k1,
    k2: state.k2,
    zoom: state.zoom,
    center: [...state.center],
  });
}

let currentStream = null;
let captureVideo = null;
let hudTimer = null;

function showHud(message, duration = 3000) {
  hud.textContent = message;
  hud.classList.add("visible");
  clearTimeout(hudTimer);
  hudTimer = setTimeout(() => hud.classList.remove("visible"), duration);
}

function signed(value) {
  return `${value >= 0 ? "+" : ""}${value.toFixed(3)}`;
}

function showState(prefix = "反畸变参数") {
  showHud(
    `${prefix} · ${state.enabled ? "已启用" : "安全旁路"} · ` +
      `k1 ${signed(state.k1)} · k2 ${signed(state.k2)} · ` +
      `缩放 ${state.zoom.toFixed(2)} · 中心 ` +
      `${state.center[0].toFixed(2)}, ${state.center[1].toFixed(2)}`,
    4200,
  );
}

function saveParameters() {
  localStorage.setItem(
    "desktop-undistort-parameters",
    JSON.stringify({
      k1: state.k1,
      k2: state.k2,
      zoom: state.zoom,
      center: state.center,
      grid: state.grid,
    }),
  );
  showState("参数已保存");
}

function stopCapture() {
  const stream = currentStream;
  currentStream = null;
  captureVideo = null;
  if (stream) {
    for (const track of stream.getTracks()) track.stop();
  }
}

async function startCapture() {
  stopCapture();
  showHud("安全旁路中 · 正在连接桌面画面…", 8000);

  try {
    const stream = await navigator.mediaDevices.getDisplayMedia({
      audio: false,
      video: {
        frameRate: { ideal: 60, max: 60 },
        cursor: "never",
      },
    });
    currentStream = stream;

    captureVideo = document.createElement("video");
    captureVideo.muted = true;
    captureVideo.playsInline = true;
    captureVideo.srcObject = stream;
    await captureVideo.play();

    const track = stream.getVideoTracks()[0];
    track.addEventListener("ended", () => {
      if (currentStream === stream) {
        window.desktopBlackHole.log("capture error: desktop capture track ended");
      }
    });

    window.desktopBlackHole.log(
      `capture started: ${captureVideo.videoWidth}x${captureVideo.videoHeight}`,
    );
    showHud(
      "桌面捕获已连接，目前为安全旁路。按 Ctrl+Alt+B 启用；任何时候按 ESC 立即退出。",
      12000,
    );
  } catch (error) {
    window.desktopBlackHole.log(`capture error: ${error.stack ?? error.message}`);
  }
}

function resizeCanvas() {
  const dpr = Math.min(window.devicePixelRatio || 1, 2);
  const width = Math.max(1, Math.round(window.innerWidth * dpr));
  const height = Math.max(1, Math.round(window.innerHeight * dpr));
  if (canvas.width !== width || canvas.height !== height) {
    canvas.width = width;
    canvas.height = height;
    gl.viewport(0, 0, width, height);
  }
}

function handleCommand(command) {
  switch (command) {
    case "toggle":
      state.enabled = !state.enabled;
      showState(state.enabled ? "反畸变已启用" : "已切回安全旁路");
      break;
    case "k1-up":
      state.k1 = Math.min(2, state.k1 + 0.05);
      showState("增加主曲率");
      break;
    case "k1-down":
      state.k1 = Math.max(-2, state.k1 - 0.05);
      showState("减小主曲率");
      break;
    case "k2-up":
      state.k2 = Math.min(3, state.k2 + 0.05);
      showState("增加边缘高阶曲率");
      break;
    case "k2-down":
      state.k2 = Math.max(-3, state.k2 - 0.05);
      showState("减小边缘高阶曲率");
      break;
    case "zoom-up":
      state.zoom = Math.min(1.6, state.zoom + 0.02);
      showState("放大预变形画面");
      break;
    case "zoom-down":
      state.zoom = Math.max(0.65, state.zoom - 0.02);
      showState("缩小预变形画面");
      break;
    case "center-up":
      state.center[1] = Math.min(0.65, state.center[1] + 0.01);
      showState("畸变中心上移");
      break;
    case "center-down":
      state.center[1] = Math.max(0.35, state.center[1] - 0.01);
      showState("畸变中心下移");
      break;
    case "center-left":
      state.center[0] = Math.max(0.35, state.center[0] - 0.01);
      showState("畸变中心左移");
      break;
    case "center-right":
      state.center[0] = Math.min(0.65, state.center[0] + 0.01);
      showState("畸变中心右移");
      break;
    case "toggle-grid":
      state.grid = !state.grid;
      showState(state.grid ? "标定网格已显示" : "标定网格已隐藏");
      break;
    case "save":
      saveParameters();
      break;
    case "reset":
      state.k1 = defaults.k1;
      state.k2 = defaults.k2;
      state.zoom = defaults.zoom;
      state.center = [...defaults.center];
      state.grid = defaults.grid;
      state.enabled = false;
      showState("已重置并切回安全旁路");
      break;
    case "help":
      showHud(
        "ESC 紧急退出 · Ctrl+Alt+B 启用/旁路 · ↑↓ 主曲率 · PgUp/PgDn 边缘曲率 · ←→ 缩放 · WASD 中心；若组合键冲突请用 Ctrl+Shift+F1～F11",
        12000,
      );
      break;
  }
  reportState();
}

window.addEventListener("resize", resizeCanvas);
canvas.addEventListener("webglcontextlost", (event) => {
  event.preventDefault();
  window.desktopBlackHole.log("capture error: WebGL context lost");
});

window.desktopBlackHole.onCommand(handleCommand);
window.desktopBlackHole.onSourceChanged(() => {
  state.enabled = false;
  startCapture();
});

function render() {
  resizeCanvas();
  gl.clearColor(0, 0, 0, 0);
  gl.clear(gl.COLOR_BUFFER_BIT);

  if (captureVideo && captureVideo.readyState >= HTMLMediaElement.HAVE_CURRENT_DATA) {
    gl.activeTexture(gl.TEXTURE0);
    gl.bindTexture(gl.TEXTURE_2D, screenTexture);
    gl.texImage2D(
      gl.TEXTURE_2D,
      0,
      gl.RGBA,
      gl.RGBA,
      gl.UNSIGNED_BYTE,
      captureVideo,
    );

    gl.useProgram(program);
    gl.uniform2f(locations.resolution, canvas.width, canvas.height);
    gl.uniform2f(locations.center, state.center[0], state.center[1]);
    gl.uniform1f(locations.k1, state.k1);
    gl.uniform1f(locations.k2, state.k2);
    gl.uniform1f(locations.zoom, state.zoom);
    gl.uniform1f(locations.grid, state.grid ? 1 : 0);
    gl.uniform1f(locations.enabled, state.enabled ? 1 : 0);
    gl.drawArrays(gl.TRIANGLES, 0, 3);
  }

  requestAnimationFrame(render);
}

resizeCanvas();
startCapture();
window.desktopBlackHole.log("undistort ready: enabled=false");
reportState();
window.desktopBlackHole.heartbeat();
setInterval(() => window.desktopBlackHole.heartbeat(), 1000);
requestAnimationFrame(render);
