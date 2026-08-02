const fs = require("node:fs");
const path = require("node:path");
const {
  app,
  BrowserWindow,
  desktopCapturer,
  dialog,
  globalShortcut,
  ipcMain,
  Menu,
  nativeImage,
  screen,
  session,
  Tray,
} = require("electron");

app.commandLine.appendSwitch("disable-renderer-backgrounding");
app.commandLine.appendSwitch("disable-backgrounding-occluded-windows");

const hasSingleInstanceLock = app.requestSingleInstanceLock();
const isUndistortMode =
  process.argv.includes("--undistort") ||
  (app.isPackaged && !process.argv.includes("--black-hole"));
const isSmokeTest = process.argv.includes("--smoke-test");
const modeLabel = isUndistortMode ? "桌面反畸变" : "桌面黑洞";
const SAFETY_TIMEOUT_MS = 15 * 60 * 1000;
const HEARTBEAT_TIMEOUT_MS = 6000;

let overlayWindow = null;
let controlWindow = null;
let tray = null;
let activeDisplayId = null;
let heartbeatTimer = null;
let safetyDeadline = Date.now() + SAFETY_TIMEOUT_MS;
let lastHeartbeat = Date.now();
let quitting = false;
let smokeTestFailed = false;
let smokeTestCaptureStarted = false;
let smokeTestSafeBypassReady = !isUndistortMode;
let smokeTestControlReady = !isUndistortMode;
let smokeTestControlButtonPassed = !isUndistortMode;
let latestEffectState = null;

const smokeLogPath = path.join(app.getPath("temp"), "desktop-black-hole-smoke.log");

function smokeLog(message) {
  if (!isSmokeTest) return;
  fs.appendFileSync(smokeLogPath, `${new Date().toISOString()} ${message}\n`);
}

if (isSmokeTest) {
  fs.writeFileSync(smokeLogPath, "");
  smokeLog(
    `started; mode=${isUndistortMode ? "undistort" : "black-hole"}; ` +
      `lock=${hasSingleInstanceLock}; argv=${JSON.stringify(process.argv)}`,
  );
}

function getActiveDisplay() {
  const displays = screen.getAllDisplays();
  const selected = displays.find((display) => display.id === activeDisplayId);
  return selected ?? screen.getDisplayNearestPoint(screen.getCursorScreenPoint());
}

async function getCaptureSource() {
  const display = getActiveDisplay();
  activeDisplayId = display.id;

  const sources = await desktopCapturer.getSources({
    types: ["screen"],
    thumbnailSize: { width: 0, height: 0 },
    fetchWindowIcons: false,
  });

  return (
    sources.find((source) => String(source.display_id) === String(display.id)) ??
    sources[0]
  );
}

function configureScreenCapture() {
  smokeLog("configuring display media request handler");
  session.defaultSession.setDisplayMediaRequestHandler((_request, callback) => {
    smokeLog("display media requested");
    getCaptureSource()
      .then((source) => {
        if (!source) {
          callback({});
          return;
        }
        smokeLog(`selected capture source ${source.id}`);
        callback({ video: source });
      })
      .catch((error) => {
        smokeTestFailed = true;
        console.error("Unable to select a screen capture source:", error);
        callback({});
      });
  });
}

function resetSafetyDeadline() {
  safetyDeadline = Date.now() + SAFETY_TIMEOUT_MS;
}

function safeExit(reason = "user requested exit") {
  if (quitting) return;
  quitting = true;
  smokeLog(`safe exit: ${reason}`);

  // Hiding first is deliberate: even if shutdown takes a moment, the real
  // desktop becomes visible and usable immediately.
  if (overlayWindow && !overlayWindow.isDestroyed()) {
    overlayWindow.hide();
  }
  if (controlWindow && !controlWindow.isDestroyed()) {
    controlWindow.hide();
  }
  setImmediate(() => app.quit());
}

function failOpenAndQuit(reason) {
  smokeTestFailed = true;
  console.error(`Safety shutdown: ${reason}`);
  safeExit(reason);
}

function sendCommand(command) {
  resetSafetyDeadline();
  if (overlayWindow && !overlayWindow.isDestroyed()) {
    overlayWindow.webContents.send("effect-command", command);
  }
}

function moveToDisplayUnderCursor() {
  resetSafetyDeadline();
  if (!overlayWindow || overlayWindow.isDestroyed()) return;

  const display = screen.getDisplayNearestPoint(screen.getCursorScreenPoint());
  activeDisplayId = display.id;
  overlayWindow.setBounds(display.bounds, false);
  overlayWindow.webContents.send("source-changed");
  positionControlPanel(display);
}

function positionControlPanel(display = getActiveDisplay()) {
  if (!controlWindow || controlWindow.isDestroyed()) return;
  const [width, height] = controlWindow.getSize();
  controlWindow.setPosition(
    display.workArea.x + display.workArea.width - width - 20,
    display.workArea.y + Math.max(20, Math.round((display.workArea.height - height) / 2)),
    false,
  );
}

function registerShortcut(accelerator, callback) {
  const registered = globalShortcut.register(accelerator, callback);
  smokeLog(`shortcut ${accelerator}: ${registered ? "registered" : "unavailable"}`);
  if (!registered) console.warn(`Global shortcut is unavailable: ${accelerator}`);
  return registered;
}

function registerShortcuts() {
  const shortcuts = isUndistortMode
    ? new Map([
        ["CommandOrControl+Alt+B", "toggle"],
        ["CommandOrControl+Alt+Up", "k1-up"],
        ["CommandOrControl+Alt+Down", "k1-down"],
        ["CommandOrControl+Alt+Right", "zoom-up"],
        ["CommandOrControl+Alt+Left", "zoom-down"],
        ["CommandOrControl+Alt+PageUp", "k2-up"],
        ["CommandOrControl+Alt+PageDown", "k2-down"],
        ["CommandOrControl+Alt+W", "center-up"],
        ["CommandOrControl+Alt+S", "center-down"],
        ["CommandOrControl+Alt+A", "center-left"],
        ["CommandOrControl+Alt+D", "center-right"],
        ["CommandOrControl+Alt+G", "toggle-grid"],
        ["CommandOrControl+Alt+P", "save"],
        ["CommandOrControl+Alt+0", "reset"],
        ["CommandOrControl+Alt+H", "help"],
      ])
    : new Map([
        ["CommandOrControl+Alt+B", "toggle"],
        ["CommandOrControl+Alt+F", "toggle-follow"],
        ["CommandOrControl+Alt+Up", "stronger"],
        ["CommandOrControl+Alt+Down", "weaker"],
        ["CommandOrControl+Alt+Right", "larger"],
        ["CommandOrControl+Alt+Left", "smaller"],
        ["CommandOrControl+Alt+0", "reset"],
        ["CommandOrControl+Alt+H", "help"],
      ]);

  for (const [accelerator, command] of shortcuts) {
    registerShortcut(accelerator, () => sendCommand(command));
  }

  // Some graphics drivers reserve Ctrl+Alt+arrow combinations. These
  // function-key fallbacks are registered in parallel so calibration always
  // retains a usable adjustment path on such machines.
  if (isUndistortMode) {
    const fallbackShortcuts = new Map([
      ["CommandOrControl+Shift+F1", "k1-down"],
      ["CommandOrControl+Shift+F2", "k1-up"],
      ["CommandOrControl+Shift+F3", "k2-down"],
      ["CommandOrControl+Shift+F4", "k2-up"],
      ["CommandOrControl+Shift+F5", "zoom-down"],
      ["CommandOrControl+Shift+F6", "zoom-up"],
      ["CommandOrControl+Shift+F7", "center-left"],
      ["CommandOrControl+Shift+F8", "center-right"],
      ["CommandOrControl+Shift+F9", "center-down"],
      ["CommandOrControl+Shift+F10", "center-up"],
      ["CommandOrControl+Shift+F11", "toggle-grid"],
    ]);
    for (const [accelerator, command] of fallbackShortcuts) {
      registerShortcut(accelerator, () => sendCommand(command));
    }
  }

  registerShortcut("CommandOrControl+Alt+M", moveToDisplayUnderCursor);
  const quitShortcutRegistered = registerShortcut(
    "CommandOrControl+Alt+Q",
    () => safeExit("Ctrl+Alt+Q"),
  );
  const escapeRegistered = isUndistortMode
    ? registerShortcut("Esc", () => safeExit("emergency Esc"))
    : false;
  const fallbackQuitRegistered = isUndistortMode
    ? registerShortcut("CommandOrControl+Shift+Alt+F12", () =>
        safeExit("fallback Ctrl+Shift+Alt+F12"),
      )
    : false;

  return quitShortcutRegistered || escapeRegistered || fallbackQuitRegistered;
}

function createTrayIcon() {
  const size = 32;
  const bitmap = Buffer.alloc(size * size * 4);
  const center = (size - 1) / 2;

  for (let y = 0; y < size; y += 1) {
    for (let x = 0; x < size; x += 1) {
      const dx = x - center;
      const dy = y - center;
      const distance = Math.sqrt(dx * dx + dy * dy);
      const ring = distance > 7.5 && distance < 13.5;
      const core = distance < 5.0;
      if (!ring && !core) continue;

      const offset = (y * size + x) * 4;
      // NativeImage bitmap channels are BGRA on Windows.
      bitmap[offset] = ring ? 245 : 28;
      bitmap[offset + 1] = ring ? 184 : 18;
      bitmap[offset + 2] = ring ? 72 : 7;
      bitmap[offset + 3] = 255;
    }
  }

  return nativeImage.createFromBitmap(bitmap, {
    width: size,
    height: size,
    scaleFactor: 1,
  });
}

function createTray() {
  try {
    tray = new Tray(createTrayIcon());
    tray.setToolTip(`${modeLabel}（右键可安全退出）`);
    tray.setContextMenu(
      Menu.buildFromTemplate([
        {
          label: isUndistortMode ? "启用/旁路反畸变" : "显示/隐藏黑洞",
          click: () => sendCommand("toggle"),
        },
        { label: "显示快捷键帮助", click: () => sendCommand("help") },
        { label: "移动到鼠标所在显示器", click: moveToDisplayUnderCursor },
        { type: "separator" },
        { label: "安全退出", click: () => safeExit("tray menu") },
      ]),
    );
    tray.on("double-click", () => sendCommand("help"));
    smokeLog("safety tray created");
    return true;
  } catch (error) {
    console.error("Unable to create safety tray icon:", error);
    return false;
  }
}

function startWatchdog() {
  lastHeartbeat = Date.now();
  heartbeatTimer = setInterval(() => {
    if (quitting) return;

    if (Date.now() - lastHeartbeat > HEARTBEAT_TIMEOUT_MS) {
      failOpenAndQuit("renderer heartbeat timeout");
      return;
    }

    if (isUndistortMode && Date.now() > safetyDeadline) {
      safeExit("15-minute calibration safety timeout");
    }
  }, 1000);
}

function createOverlay() {
  const display = screen.getDisplayNearestPoint(screen.getCursorScreenPoint());
  activeDisplayId = display.id;

  overlayWindow = new BrowserWindow({
    ...display.bounds,
    show: false,
    frame: false,
    transparent: true,
    backgroundColor: "#00000000",
    hasShadow: false,
    roundedCorners: false,
    resizable: false,
    movable: false,
    minimizable: false,
    maximizable: false,
    closable: true,
    focusable: false,
    skipTaskbar: true,
    fullscreenable: false,
    webPreferences: {
      preload: path.join(__dirname, "preload.js"),
      contextIsolation: true,
      nodeIntegration: false,
      sandbox: true,
      backgroundThrottling: false,
      devTools: !app.isPackaged,
    },
  });
  smokeLog(`overlay created on display ${display.id}`);

  // Windows 10 2004+ removes this overlay from desktop capture. This avoids
  // recursive feedback while the real desktop stays clickable underneath.
  overlayWindow.setContentProtection(true);
  overlayWindow.setIgnoreMouseEvents(true, { forward: true });
  overlayWindow.setAlwaysOnTop(true, "screen-saver", 1);
  overlayWindow.setVisibleOnAllWorkspaces(true, { visibleOnFullScreen: true });

  const page = isUndistortMode ? "undistort.html" : "index.html";
  overlayWindow.loadFile(path.join(__dirname, page));
  overlayWindow.once("ready-to-show", () => {
    smokeLog("overlay ready-to-show");
    overlayWindow?.showInactive();
  });
  overlayWindow.webContents.on("did-finish-load", () => smokeLog("page did-finish-load"));
  overlayWindow.webContents.on("console-message", (_event, details) => {
    console.log(`[web:${details.level}] ${details.message}`);
  });
  overlayWindow.webContents.on("did-fail-load", (_event, code, description) => {
    failOpenAndQuit(`page load failed (${code}): ${description}`);
  });
  overlayWindow.webContents.on("render-process-gone", (_event, details) => {
    if (!quitting) failOpenAndQuit(`renderer stopped: ${details.reason}`);
  });
  overlayWindow.webContents.on("unresponsive", () => {
    failOpenAndQuit("renderer became unresponsive");
  });
  overlayWindow.on("closed", () => {
    smokeLog("overlay closed");
    overlayWindow = null;
  });
}

function createControlPanel() {
  const display = getActiveDisplay();
  const width = 430;
  const height = Math.min(760, display.workArea.height - 40);

  controlWindow = new BrowserWindow({
    width,
    height,
    show: false,
    title: "桌面反畸变控制台",
    frame: true,
    transparent: false,
    backgroundColor: "#081018",
    resizable: false,
    minimizable: true,
    maximizable: false,
    fullscreenable: false,
    skipTaskbar: false,
    webPreferences: {
      preload: path.join(__dirname, "preload.js"),
      contextIsolation: true,
      nodeIntegration: false,
      sandbox: true,
      backgroundThrottling: false,
      devTools: !app.isPackaged,
    },
  });

  // The panel must not be fed back into the desktop texture either.
  controlWindow.setContentProtection(true);
  controlWindow.setAlwaysOnTop(true, "screen-saver", 2);
  positionControlPanel(display);
  controlWindow.loadFile(path.join(__dirname, "undistort-control.html"));

  controlWindow.once("ready-to-show", () => {
    smokeLog("control panel ready-to-show");
    controlWindow?.show();
    controlWindow?.focus();
  });
  controlWindow.webContents.on("did-finish-load", () => {
    smokeLog("control panel did-finish-load");
    smokeTestControlReady = true;
    if (latestEffectState && controlWindow) {
      controlWindow.webContents.send("effect-state", latestEffectState);
    }
    if (isSmokeTest) {
      setTimeout(() => {
        controlWindow?.webContents
          .executeJavaScript(
            'document.querySelector("[data-command=toggle]").click()',
          )
          .catch((error) => {
            smokeTestFailed = true;
            smokeLog(`control button smoke failed: ${error.message}`);
          });
      }, 2500);
    }
  });
  controlWindow.webContents.on("did-fail-load", (_event, code, description) => {
    failOpenAndQuit(`control panel load failed (${code}): ${description}`);
  });
  controlWindow.webContents.on("render-process-gone", (_event, details) => {
    if (!quitting) failOpenAndQuit(`control panel renderer stopped: ${details.reason}`);
  });
  controlWindow.on("close", (event) => {
    if (!quitting) {
      event.preventDefault();
      safeExit("control panel close button");
    }
  });
  controlWindow.on("closed", () => {
    smokeLog("control panel closed");
    controlWindow = null;
  });
}

if (!hasSingleInstanceLock) {
  app.quit();
} else {
  app.on("second-instance", () => sendCommand("help"));

  app.whenReady().then(() => {
    smokeLog("app ready");
    configureScreenCapture();

    const hasExitShortcut = registerShortcuts();
    const hasTray = createTray();
    if (isUndistortMode && !hasExitShortcut && !hasTray) {
      dialog.showErrorBox(
        "无法安全启动",
        "紧急退出快捷键和系统托盘均创建失败，反畸变覆盖层未启动。",
      );
      app.quit();
      return;
    }

    createOverlay();
    if (isUndistortMode) createControlPanel();
    startWatchdog();

    if (isSmokeTest) {
      setTimeout(() => {
        if (
          !smokeTestCaptureStarted ||
          !smokeTestSafeBypassReady ||
          !smokeTestControlReady ||
          !smokeTestControlButtonPassed
        ) {
          smokeTestFailed = true;
          console.error("Smoke test did not complete all safety handshakes.");
        }
        process.exitCode = smokeTestFailed ? 1 : 0;
        smokeLog(
          `smoke timeout; capture=${smokeTestCaptureStarted}; failed=${smokeTestFailed}`,
        );
        safeExit("smoke test complete");
      }, 12000);
    }

    screen.on("display-metrics-changed", (_event, display) => {
      if (display.id === activeDisplayId && overlayWindow) {
        overlayWindow.setBounds(display.bounds, false);
        overlayWindow.webContents.send("source-changed");
        positionControlPanel(display);
      }
    });
  });

  app.on("will-quit", () => {
    smokeLog("will-quit");
    clearInterval(heartbeatTimer);
    globalShortcut.unregisterAll();
    tray?.destroy();
    tray = null;
  });
  app.on("window-all-closed", () => {
    smokeLog("window-all-closed");
    if (!quitting) safeExit("overlay window closed");
  });
}

ipcMain.on("renderer-heartbeat", () => {
  lastHeartbeat = Date.now();
});

const allowedControlCommands = new Set([
  "toggle",
  "k1-up",
  "k1-down",
  "k2-up",
  "k2-down",
  "zoom-up",
  "zoom-down",
  "center-up",
  "center-down",
  "center-left",
  "center-right",
  "toggle-grid",
  "save",
  "reset",
  "help",
]);

ipcMain.on("control-command", (_event, command) => {
  if (!isUndistortMode || typeof command !== "string") return;
  if (command === "exit") {
    safeExit("control panel exit button");
    return;
  }
  if (allowedControlCommands.has(command)) sendCommand(command);
});

ipcMain.on("effect-state", (_event, state) => {
  if (!isUndistortMode || !state || typeof state !== "object") return;
  latestEffectState = {
    enabled: Boolean(state.enabled),
    grid: Boolean(state.grid),
    k1: Number(state.k1) || 0,
    k2: Number(state.k2) || 0,
    zoom: Number(state.zoom) || 1,
    center: Array.isArray(state.center)
      ? [Number(state.center[0]) || 0.5, Number(state.center[1]) || 0.5]
      : [0.5, 0.5],
  };
  smokeLog(
    `effect state: enabled=${latestEffectState.enabled}; ` +
      `k1=${latestEffectState.k1}; k2=${latestEffectState.k2}`,
  );
  if (isSmokeTest && latestEffectState.enabled && !smokeTestControlButtonPassed) {
    smokeTestControlButtonPassed = true;
    smokeLog("control button round-trip passed");
    sendCommand("toggle");
  }
  if (controlWindow && !controlWindow.isDestroyed()) {
    controlWindow.webContents.send("effect-state", latestEffectState);
  }
});

ipcMain.on("renderer-log", (_event, message) => {
  smokeLog(`renderer: ${message}`);
  console.log(`[renderer] ${message}`);

  if (message.startsWith("capture started:")) {
    smokeTestCaptureStarted = true;
  }
  if (message === "undistort ready: enabled=false") {
    smokeTestSafeBypassReady = true;
  }
  if (message.startsWith("capture error:")) {
    smokeTestFailed = true;
    failOpenAndQuit(message);
  }
});
