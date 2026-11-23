"use strict";

/* Applied before first paint: "system" (default), "light" or "dark". */
(function applyStoredTheme() {
  try {
    const stored = localStorage.getItem("fates-theme");
    if (stored === "light" || stored === "dark") {
      document.documentElement.dataset.theme = stored;
    }
  } catch {
  }
})();
