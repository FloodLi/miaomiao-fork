(function () {
  if (document.getElementById("feature-help-style-v3")) return;

  var style = document.createElement("style");
  style.id = "feature-help-style-v3";
  style.textContent =
    ".help-label,.help-heading,.label-with-info,.heading-with-info,.switch-title-with-info{" +
    "display:inline-flex;align-items:center;gap:4px;min-width:0}" +
    ".info-dot{position:relative;display:inline-grid;place-items:center;flex:0 0 auto;" +
    "width:28px;height:28px;margin:-5px -4px;padding:0;border:0;border-radius:50%;" +
    "background:transparent;color:var(--ui-muted,var(--t2,var(--muted,#7b6972)));" +
    "cursor:help;touch-action:manipulation;vertical-align:middle}" +
    ".info-dot::before{content:'i';display:grid;place-items:center;width:17px;height:17px;" +
    "border:1.5px solid currentColor;border-radius:50%;" +
    "background:var(--ui-card,var(--card,var(--sf,#fff)));font:900 11px/1 Arial,sans-serif;letter-spacing:0}" +
    ".info-dot:hover,.info-dot:focus-visible{color:var(--ui-accent,var(--ac,var(--accent,#e83d68)));outline:0}" +
    ".info-dot:focus-visible::before{box-shadow:0 0 0 3px rgba(232,61,104,.16)}" +
    ".feature-help-popover{position:fixed;z-index:10050;width:max-content;" +
    "max-width:min(300px,calc(100vw - 24px));padding:10px 12px;border:1px solid rgba(255,255,255,.18);" +
    "border-radius:8px;background:#2d2428;color:#fff;box-shadow:0 12px 28px rgba(35,24,30,.22);" +
    "font-size:13px;font-weight:600;line-height:1.65;letter-spacing:0;pointer-events:none}" +
    ".feature-help-popover[hidden]{display:none!important}" +
    ".more-settings{width:100%;margin:12px 0;border:1px solid var(--ui-line,var(--sb,var(--cb,#eadde3)));" +
    "border-radius:12px;background:var(--ui-card,var(--card,var(--sf,rgba(255,255,255,.56))));overflow:hidden}" +
    ".more-settings>summary{display:flex;align-items:center;justify-content:space-between;gap:12px;" +
    "min-height:48px;padding:0 14px;cursor:pointer;list-style:none;color:var(--ui-text,var(--tx,#2d2428));" +
    "font-size:14px;font-weight:800;letter-spacing:0;user-select:none}" +
    ".more-settings>summary::-webkit-details-marker{display:none}" +
    ".more-settings>summary::after{content:'展开';flex:0 0 auto;color:var(--ui-muted,var(--t2,#9a7888));font-size:13px;font-weight:700}" +
    ".more-settings[open]>summary::after{content:'收起'}" +
    ".more-settings>summary:hover{background:var(--ui-hover,var(--hv,rgba(255,255,255,.5)))}" +
    ".more-settings>summary:focus-visible{outline:2px solid var(--ui-accent,var(--ac,#e83d68));outline-offset:-3px}" +
    ".more-settings-body{display:grid;gap:12px;padding:14px;border-top:1px solid var(--ui-line,var(--sb,var(--cb,#eadde3)))}" +
    ".more-settings-body>.form-row,.more-settings-body>.field-group{margin-bottom:0!important}" +
    "@media(max-width:760px){.info-dot{width:44px;height:44px;margin:-10px -8px -10px -6px}" +
    ".more-settings>summary{min-height:52px;padding:0 14px;font-size:15px}.more-settings-body{padding:13px}}";
  document.head.appendChild(style);

  var pop = document.createElement("div");
  pop.className = "feature-help-popover";
  pop.id = "featureHelpPopover";
  pop.setAttribute("role", "tooltip");
  pop.hidden = true;
  document.body.appendChild(pop);

  var active = null;
  var pinned = false;

  function place(button) {
    var rect = button.getBoundingClientRect();
    var width = pop.offsetWidth;
    var height = pop.offsetHeight;
    var gap = 8;
    var left = rect.left + rect.width / 2 - width / 2;
    left = Math.max(12, Math.min(left, window.innerWidth - width - 12));
    var top = rect.bottom + gap;
    if (top + height > window.innerHeight - 12 && rect.top - height - gap >= 12) {
      top = rect.top - height - gap;
    }
    pop.style.left = Math.round(left) + "px";
    pop.style.top = Math.round(top) + "px";
  }

  function hide() {
    if (active) active.setAttribute("aria-expanded", "false");
    active = null;
    pinned = false;
    pop.hidden = true;
  }

  function show(button, keepOpen) {
    if (active && active !== button) active.setAttribute("aria-expanded", "false");
    active = button;
    pinned = !!keepOpen;
    pop.textContent = button.dataset.help || "";
    pop.hidden = false;
    button.setAttribute("aria-expanded", "true");
    requestAnimationFrame(function () { place(button); });
  }

  document.querySelectorAll(".info-dot[data-help]").forEach(function (button) {
    button.addEventListener("mouseenter", function () { if (!pinned) show(button, false); });
    button.addEventListener("mouseleave", function () { if (!pinned) hide(); });
    button.addEventListener("click", function (event) {
      event.preventDefault();
      event.stopPropagation();
      if (active === button && pinned) hide();
      else show(button, true);
    });
  });

  document.addEventListener("click", hide);
  document.addEventListener("keydown", function (event) { if (event.key === "Escape") hide(); });
  window.addEventListener("resize", function () { if (active) place(active); });
  window.addEventListener("scroll", function () { if (active) hide(); }, true);

  window.epdExpandMoreSettings = function (target) {
    var node = typeof target === "string" ? document.getElementById(target) : target;
    var details = node && node.matches && node.matches("details.more-settings")
      ? node
      : node && node.closest ? node.closest("details.more-settings") : null;
    if (!details) return false;
    details.open = true;
    return true;
  };
})();
