const status = document.querySelector("#status");
const toggle = document.querySelector("#toggle");
const grid = document.querySelector("#grid");
const k1 = document.querySelector("#k1");
const k2 = document.querySelector("#k2");
const zoom = document.querySelector("#zoom");
const center = document.querySelector("#center");

function signed(value) {
  return `${value >= 0 ? "+" : ""}${value.toFixed(3)}`;
}

function updateState(state) {
  status.textContent = state.enabled ? "反畸变已启用" : "安全旁路";
  status.classList.toggle("active", state.enabled);
  status.classList.toggle("bypass", !state.enabled);

  toggle.textContent = state.enabled ? "切回安全旁路" : "启用反畸变";
  toggle.classList.toggle("active", state.enabled);
  grid.textContent = state.grid ? "隐藏网格" : "显示网格";

  k1.textContent = signed(state.k1);
  k2.textContent = signed(state.k2);
  zoom.textContent = `${state.zoom.toFixed(2)}×`;
  center.textContent = `${state.center[0].toFixed(2)}, ${state.center[1].toFixed(2)}`;
}

for (const button of document.querySelectorAll("[data-command]")) {
  button.addEventListener("click", () => {
    window.desktopBlackHole.sendCommand(button.dataset.command);
  });
}

window.desktopBlackHole.onState(updateState);
