const { contextBridge, ipcRenderer } = require("electron");

contextBridge.exposeInMainWorld("desktopBlackHole", {
  onCommand(callback) {
    ipcRenderer.on("effect-command", (_event, command) => callback(command));
  },
  onSourceChanged(callback) {
    ipcRenderer.on("source-changed", () => callback());
  },
  log(message) {
    ipcRenderer.send("renderer-log", String(message));
  },
  heartbeat() {
    ipcRenderer.send("renderer-heartbeat");
  },
  sendCommand(command) {
    ipcRenderer.send("control-command", String(command));
  },
  reportState(state) {
    ipcRenderer.send("effect-state", state);
  },
  onState(callback) {
    ipcRenderer.on("effect-state", (_event, state) => callback(state));
  },
});
