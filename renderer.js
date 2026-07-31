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
  uniform float u_time;
  uniform float u_radius;
  uniform float u_strength;
  uniform float u_enabled;

  varying vec2 v_uv;

  const float PI = 3.14159265359;

  mat2 rotate2d(float angle) {
    float s = sin(angle);
    float c = cos(angle);
    return mat2(c, -s, s, c);
  }

  float hash21(vec2 p) {
    p = fract(p * vec2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return fract(p.x * p.y);
  }

  float noise21(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);

    return mix(
      mix(hash21(i), hash21(i + vec2(1.0, 0.0)), f.x),
      mix(hash21(i + vec2(0.0, 1.0)), hash21(i + vec2(1.0, 1.0)), f.x),
      f.y
    );
  }

  float fbm(vec2 p) {
    float value = 0.0;
    float amplitude = 0.5;
    for (int i = 0; i < 4; i++) {
      value += amplitude * noise21(p);
      p = rotate2d(0.73) * p * 2.03 + 13.7;
      amplitude *= 0.5;
    }
    return value;
  }

  float gaussian(float value, float width) {
    float x = value / max(width, 0.00001);
    return exp(-x * x);
  }

  void main() {
    vec2 aspect = vec2(u_resolution.x / u_resolution.y, 1.0);
    vec2 p = (v_uv - u_center) * aspect;
    float r = length(p);
    float safeR = max(r, 0.0001);
    vec2 radial = p / safeR;

    float horizon = u_radius;
    float influenceRadius = horizon * 5.3;
    float lensMask = 1.0 - smoothstep(horizon * 1.02, influenceRadius, r);

    // A fast screen-space approximation of gravitational lensing. Output
    // pixels sample increasingly distant screen pixels near the event horizon.
    float inverseField = (horizon * horizon) /
      (safeR * safeR + horizon * horizon * 0.22);
    float bend = u_strength * 0.34 * inverseField * lensMask;
    float swirl = 0.30 * lensMask * lensMask +
      0.018 * sin(u_time * 0.7 + r / horizon * 7.0) * lensMask;

    vec2 warpedP = rotate2d(swirl) * p * (1.0 + bend);
    vec2 warpedUv = u_center + warpedP / aspect;
    vec2 chroma = (radial / aspect) * 0.0034 * lensMask * u_strength;

    vec3 scene;
    scene.r = texture2D(u_screen, clamp(warpedUv + chroma, 0.0, 1.0)).r;
    scene.g = texture2D(u_screen, clamp(warpedUv, 0.0, 1.0)).g;
    scene.b = texture2D(u_screen, clamp(warpedUv - chroma, 0.0, 1.0)).b;

    // Accretion disc: an animated, inclined procedural annulus.
    vec2 discP = rotate2d(-0.20) * p;
    vec2 discSpace = vec2(discP.x, discP.y / 0.235);
    float discRadius = length(discSpace);
    float discAngle = atan(discSpace.y, discSpace.x);
    float innerDisc = horizon * 1.22;
    float outerDisc = horizon * 3.35;
    float discWindow = smoothstep(innerDisc, innerDisc + horizon * 0.14, discRadius) *
      (1.0 - smoothstep(outerDisc - horizon * 0.32, outerDisc, discRadius));

    float flow = fbm(vec2(
      discRadius / horizon * 3.2,
      discAngle * 2.8 - u_time * 0.82
    ));
    float fineFlow = fbm(vec2(
      discRadius / horizon * 8.0 - u_time * 0.18,
      discAngle * 7.0 + u_time * 1.35
    ));
    float bands = pow(
      0.5 + 0.5 * cos(discRadius / horizon * 52.0 + flow * 8.0),
      5.0
    );
    float filaments = pow(
      0.5 + 0.5 * sin(discRadius / horizon * 91.0 - fineFlow * 11.0),
      12.0
    );
    float discTexture = discWindow * (0.12 + bands * 0.64 + filaments * 0.85);

    // The approaching side is brighter, suggesting relativistic beaming.
    float doppler = mix(0.42, 1.72, 0.5 + 0.5 * cos(discAngle - 0.25));
    float heat = 1.0 - smoothstep(innerDisc, outerDisc, discRadius);
    vec3 coolDisc = vec3(0.22, 0.42, 0.92);
    vec3 hotDisc = vec3(1.0, 0.78, 0.36);
    vec3 whiteHot = vec3(1.0, 0.97, 0.88);
    vec3 discColor = mix(coolDisc, hotDisc, smoothstep(0.0, 0.72, heat));
    discColor = mix(discColor, whiteHot, pow(heat, 3.0));

    float broadDiscGlow = discWindow * (0.10 + 0.22 * flow) * doppler;
    float discLight = discTexture * doppler;

    // Deep shadow and the narrow photon ring around the event horizon.
    float shadow = 1.0 - smoothstep(horizon * 0.98, horizon * 1.48, r);
    float core = 1.0 - smoothstep(horizon * 0.92, horizon * 1.025, r);
    float photonRing = gaussian(r - horizon * 1.075, horizon * 0.034);
    float outerHalo = gaussian(r - horizon * 1.18, horizon * 0.16) * 0.16;

    vec3 color = scene * (1.0 - shadow * 0.82);
    color += discColor * broadDiscGlow;
    color += discColor * discLight * 1.85;
    color += vec3(0.76, 0.87, 1.0) * (photonRing * 1.75 + outerHalo);
    color = mix(color, vec3(0.0002, 0.0005, 0.0015), core);

    // Subtle exposure compression keeps the brightest filaments detailed.
    color = 1.0 - exp(-color * 1.18);

    float region = 1.0 - smoothstep(horizon * 4.55, influenceRadius, r);
    float alpha = max(region, max(discWindow * 0.93, photonRing));
    alpha = clamp(alpha * u_enabled, 0.0, 1.0);

    gl_FragColor = vec4(color * alpha, alpha);
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
  time: gl.getUniformLocation(program, "u_time"),
  radius: gl.getUniformLocation(program, "u_radius"),
  strength: gl.getUniformLocation(program, "u_strength"),
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
  radius: 0.105,
  strength: 1.0,
  follow: true,
  enabled: true,
};

const state = {
  ...defaults,
  center: [0.5, 0.5],
  target: [0.5, 0.5],
};

let currentStream = null;
let captureVideo = null;
let hudTimer = null;
let lastFrameTime = performance.now();
const startTime = performance.now();

function showHud(message, duration = 2200) {
  hud.textContent = message;
  hud.classList.add("visible");
  clearTimeout(hudTimer);
  hudTimer = setTimeout(() => hud.classList.remove("visible"), duration);
}

function showState(prefix = "黑洞") {
  showHud(
    `${prefix} · 大小 ${Math.round(state.radius * 1000)} · ` +
      `引力 ${state.strength.toFixed(2)} · ` +
      `${state.follow ? "跟随鼠标" : "位置已固定"}`,
  );
}

function stopCapture() {
  if (currentStream) {
    for (const track of currentStream.getTracks()) track.stop();
  }
  currentStream = null;
  captureVideo = null;
}

async function startCapture() {
  stopCapture();
  showHud("正在连接桌面画面…", 5000);

  try {
    currentStream = await navigator.mediaDevices.getDisplayMedia({
      audio: false,
      video: {
        frameRate: { ideal: 60, max: 60 },
        cursor: "never",
      },
    });

    captureVideo = document.createElement("video");
    captureVideo.muted = true;
    captureVideo.playsInline = true;
    captureVideo.srcObject = currentStream;
    await captureVideo.play();

    const track = currentStream.getVideoTracks()[0];
    track.addEventListener("ended", () => {
      showHud("桌面捕获已停止，请重新启动程序。", 10000);
    });

    showState("桌面捕获已连接");
    window.desktopBlackHole.log(
      `capture started: ${captureVideo.videoWidth}x${captureVideo.videoHeight}`,
    );
  } catch (error) {
    showHud(`无法捕获桌面：${error.message}`, 12000);
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

window.addEventListener("resize", resizeCanvas);
window.addEventListener("mousemove", (event) => {
  if (!state.follow) return;
  state.target[0] = event.clientX / window.innerWidth;
  state.target[1] = 1.0 - event.clientY / window.innerHeight;
});

function handleCommand(command) {
  switch (command) {
    case "toggle":
      state.enabled = !state.enabled;
      showState(state.enabled ? "黑洞已显示" : "黑洞已隐藏");
      break;
    case "toggle-follow":
      state.follow = !state.follow;
      showState(state.follow ? "已恢复跟随" : "位置已固定");
      break;
    case "stronger":
      state.strength = Math.min(2.2, state.strength + 0.1);
      showState("增强引力");
      break;
    case "weaker":
      state.strength = Math.max(0.2, state.strength - 0.1);
      showState("减弱引力");
      break;
    case "larger":
      state.radius = Math.min(0.19, state.radius + 0.008);
      showState("放大黑洞");
      break;
    case "smaller":
      state.radius = Math.max(0.045, state.radius - 0.008);
      showState("缩小黑洞");
      break;
    case "reset":
      state.radius = defaults.radius;
      state.strength = defaults.strength;
      state.follow = defaults.follow;
      state.enabled = defaults.enabled;
      showState("参数已重置");
      break;
    case "help":
      showHud(
        "Ctrl+Alt+B 显示/隐藏 · F 固定 · 方向键调节 · M 切换屏幕 · Q 退出",
        7000,
      );
      break;
  }
}

window.desktopBlackHole.onCommand(handleCommand);
window.desktopBlackHole.onSourceChanged(() => {
  state.center = [0.5, 0.5];
  state.target = [0.5, 0.5];
  startCapture();
});

function render(now) {
  resizeCanvas();

  const deltaSeconds = Math.min((now - lastFrameTime) / 1000, 0.1);
  lastFrameTime = now;
  const smoothing = 1.0 - Math.exp(-deltaSeconds * 12.0);
  state.center[0] += (state.target[0] - state.center[0]) * smoothing;
  state.center[1] += (state.target[1] - state.center[1]) * smoothing;

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
    gl.uniform1f(locations.time, (now - startTime) / 1000);
    gl.uniform1f(locations.radius, state.radius);
    gl.uniform1f(locations.strength, state.strength);
    gl.uniform1f(locations.enabled, state.enabled ? 1 : 0);
    gl.drawArrays(gl.TRIANGLES, 0, 3);
  }

  requestAnimationFrame(render);
}

resizeCanvas();
startCapture();
window.desktopBlackHole.heartbeat();
setInterval(() => window.desktopBlackHole.heartbeat(), 1000);
requestAnimationFrame(render);
