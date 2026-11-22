"use strict";
(() => {
  const output = document.getElementById("document");
  const md = window.markdownit({html: false});
  const normalFence = md.renderer.rules.fence;
  md.renderer.rules.fence = (tokens, index, options, env, self) => {
    const token = tokens[index];
    if (token.info.trim().split(/\s+/)[0].toLowerCase() !== "mermaid")
      return normalFence(tokens, index, options, env, self);
    return '<div class="diagram"><pre>' + md.utils.escapeHtml(token.content) + '</pre></div>\n';
  };
  md.renderer.rules.table_open = () => '<div class="table-scroll"><table>\n';
  md.renderer.rules.table_close = () => '</table></div>\n';

  let revision = 0, library, work = Promise.resolve();
  function loadDiagrams() {
    if (!library) library = new Promise((resolve, reject) => {
      const script = document.createElement("script");
      script.src = "vendor/mermaid.min.js";
      script.onload = () => resolve(window.mermaid);
      script.onerror = () => { library = null; reject(new Error("Diagram renderer unavailable")); };
      document.head.appendChild(script);
    });
    return library;
  }

  function fallback(box, source, message) {
    const detail = document.createElement("details");
    const label = document.createElement("summary");
    const code = document.createElement("pre");
    label.textContent = message;
    code.textContent = source;
    detail.append(label, code);
    box.replaceChildren(detail);
  }

  const rootSize = () => parseFloat(getComputedStyle(document.documentElement).fontSize) || 16;

  // Mermaid reads ";" as a statement separator, so "A->>B: x; then y" fails to
  // parse. In a sequence diagram, escape it as Mermaid's "#59;" entity inside
  // message and note text only (after the first ":"), keeping existing entities.
  const arrow = /(<<)?--?(>>|>|x|\))/;
  const note = /^\s*note\s+(left\s+of|right\s+of|over)\s/i;
  function lenientSequence(source) {
    const lines = source.split("\n");
    const first = lines.find(line => line.trim());
    if (!first || !/^\s*sequenceDiagram\s*$/.test(first) || !source.includes(";")) return source;
    return lines.map(line => {
      const colon = line.indexOf(":");
      if (colon < 0 || !line.includes(";", colon) || /^\s*%%/.test(line)) return line;
      const head = line.slice(0, colon);
      if (!note.test(head) && !arrow.test(head)) return line;
      return head + line.slice(colon).replace(/#\w+;|;/g, match => match === ";" ? "#59;" : match);
    }).join("\n");
  }

  // Zoom and pan. One set of listeners on the document container serves every
  // diagram, so replacing the document leaves nothing behind; nothing runs while idle.
  const MIN_ZOOM = 0.25, MAX_ZOOM = 4, STEP = 1.25;
  const zooms = new WeakMap();
  const clampZoom = scale => Math.min(MAX_ZOOM, Math.max(MIN_ZOOM, scale));
  let drag = null, gesture = null, frame = 0, queued = null;

  function zoomable(box) {
    const image = box.querySelector("svg");
    const size = image?.viewBox?.baseVal;
    if (!size || !(size.width > 0 && size.height > 0)) return;
    image.removeAttribute("width");
    image.removeAttribute("height");
    image.style.maxWidth = "none";
    const view = document.createElement("div");
    view.className = "diagram-view";
    view.append(image);
    view.addEventListener("wheel", wheel, {passive: false});
    const tools = document.createElement("div");
    tools.className = "diagram-tools";
    tools.setAttribute("role", "toolbar");
    tools.setAttribute("aria-label", "Diagram zoom");
    for (const [action, text, label] of [["out", "−", "Zoom out"], ["in", "+", "Zoom in"],
                                          ["fit", "Fit", "Fit to width"], ["reset", "100%", "Reset zoom to 100%"]]) {
      const button = document.createElement("button");
      button.type = "button";
      button.dataset.zoom = action;
      button.textContent = text;
      button.title = label;
      button.setAttribute("aria-label", label);
      tools.append(button);
    }
    box.replaceChildren(view, tools);
    const state = {image, width: size.width, height: size.height, scale: 1, label: tools.lastChild};
    zooms.set(view, state);
    // Natural size when it fits; otherwise fit to width, but never so small that
    // labels drop under 10px. Then the user pans.
    const room = view.clientWidth;
    let scale = 1;
    if (room > 0 && state.width > room)
      scale = Math.min(1, Math.max(room / state.width, 10 / rootSize()));
    setZoom(view, scale);
  }

  // Resize the SVG itself (text stays vector-crisp, scroll extents stay right) and
  // keep the content under (x, y) in place when a point is given.
  function setZoom(view, scale, x, y) {
    const state = zooms.get(view);
    if (!state) return;
    scale = clampZoom(scale);
    const image = state.image, anchored = Number.isFinite(x) && Number.isFinite(y);
    const before = anchored && image.getBoundingClientRect();
    image.style.width = state.width * scale + "px";
    image.style.height = state.height * scale + "px";
    state.scale = scale;
    if (anchored && before.width > 0 && before.height > 0) {
      const after = image.getBoundingClientRect();
      view.scrollLeft += after.left + (x - before.left) / before.width * after.width - x;
      view.scrollTop += after.top + (y - before.top) / before.height * after.height - y;
    }
    view.dataset.scale = String(scale);
    state.label.textContent = Math.round(scale * 100) + "%";
    view.classList.toggle("pannable", pannable(view));
  }
  const pannable = view => view.scrollWidth > view.clientWidth + 1 || view.scrollHeight > view.clientHeight + 1;

  // Coalesce pinch and wheel input to one resize per frame, only while it arrives.
  function queueZoom(view, scale, x, y) {
    queued = {view, scale: clampZoom(scale), x, y};
    if (!frame) frame = requestAnimationFrame(() => {
      const next = queued;
      frame = 0;
      queued = null;
      if (next?.view.isConnected) setZoom(next.view, next.scale, next.x, next.y);
    });
  }
  const pendingScale = (view, state) => queued?.view === view ? queued.scale : state.scale;
  const viewAt = target => target?.closest?.(".diagram-view");

  function zoomControl(button) {
    const view = button.closest(".diagram")?.querySelector(".diagram-view");
    const state = view && zooms.get(view);
    if (!state) return;
    const action = button.dataset.zoom;
    let scale = 1;
    if (action === "in" || action === "out") {
      scale = action === "in" ? state.scale * STEP : state.scale / STEP;
      if (Math.abs(scale - 1) < 0.04) scale = 1;
    } else if (action === "fit") {
      scale = view.clientWidth > 0 ? view.clientWidth / state.width : state.scale;
    }
    const rect = view.getBoundingClientRect();
    setZoom(view, scale, rect.left + rect.width / 2, rect.top + rect.height / 2);
  }

  // A trackpad pinch arrives as ctrl+wheel; a plain wheel or two-finger scroll
  // is left alone so it scrolls the box or the page as usual. Bound per diagram
  // (and dropped with it) so the rest of the page keeps passive, fast scrolling.
  function wheel(event) {
    if (!event.ctrlKey) return;
    const view = event.currentTarget, state = zooms.get(view);
    if (!state) return;
    event.preventDefault();
    if (gesture) return;
    const delta = event.deltaY * (event.deltaMode === 1 ? 16 : event.deltaMode === 2 ? 400 : 1);
    const factor = Math.exp(-Math.max(-25, Math.min(25, delta)) / 100);
    queueZoom(view, pendingScale(view, state) * factor, event.clientX, event.clientY);
  }
  // Safari's pinch gestures (iOS touch; macOS alongside ctrl+wheel).
  output.addEventListener("gesturestart", event => {
    const view = viewAt(event.target), state = view && zooms.get(view);
    if (!state) return;
    event.preventDefault();
    gesture = {view, base: state.scale};
  }, {passive: false});
  output.addEventListener("gesturechange", event => {
    if (!gesture) return;
    event.preventDefault();
    queueZoom(gesture.view, gesture.base * event.scale, event.clientX, event.clientY);
  }, {passive: false});
  output.addEventListener("gestureend", () => { gesture = null; });

  // Drag to pan with a mouse or pen; touch keeps native scrolling.
  output.addEventListener("pointerdown", event => {
    if (event.pointerType === "touch" || event.button !== 0) return;
    const view = viewAt(event.target);
    if (!view || !zooms.has(view)) return;
    const canPan = pannable(view);
    view.classList.toggle("pannable", canPan);
    if (!canPan) return;
    drag = {view, id: event.pointerId, x: event.clientX, y: event.clientY, left: view.scrollLeft, top: view.scrollTop};
    view.setPointerCapture(event.pointerId);
    view.classList.add("dragging");
    event.preventDefault();
  });
  output.addEventListener("pointermove", event => {
    if (drag?.id !== event.pointerId) return;
    drag.view.scrollLeft = drag.left - (event.clientX - drag.x);
    drag.view.scrollTop = drag.top - (event.clientY - drag.y);
  });
  const endDrag = event => {
    if (drag?.id !== event.pointerId) return;
    drag.view.classList.remove("dragging");
    drag = null;
  };
  for (const type of ["pointerup", "pointercancel", "lostpointercapture"]) output.addEventListener(type, endDrag);
  output.addEventListener("dblclick", event => {
    const view = viewAt(event.target);
    if (!view || !zooms.has(view)) return;
    event.preventDefault();
    setZoom(view, 1, event.clientX, event.clientY);
  });

  async function diagrams(boxes, theme, version) {
    if (version !== revision) return;
    const api = await loadDiagrams();
    if (version !== revision) return;
    const px = rootSize();
    api.initialize({
      startOnLoad: false, securityLevel: "strict", theme: "base", look: "classic",
      htmlLabels: false, maxTextSize: 16384, maxEdges: 200, suppressErrorRendering: true,
      // Labels in the document's font at its text size, not Mermaid's fixed 16px.
      fontSize: px, sequence: {actorFontSize: px, messageFontSize: px, noteFontSize: px},
      themeVariables: {
        fontSize: px + "px", fontFamily: getComputedStyle(document.body).fontFamily,
        darkMode: theme.dark, background: "transparent", primaryColor: theme.surface,
        primaryTextColor: theme.text, primaryBorderColor: theme.accent, lineColor: theme.muted,
        textColor: theme.text, secondaryColor: theme.surface, tertiaryColor: theme.surface,
        actorBkg: theme.surface, actorBorder: theme.accent, actorTextColor: theme.text,
        actorLineColor: theme.muted, signalColor: theme.muted, signalTextColor: theme.text,
        edgeLabelBackground: theme.background, noteBkgColor: theme.surface, noteTextColor: theme.text
      }
    });
    for (const [i, box] of boxes.entries()) {
      if (version !== revision) return;
      const source = box.textContent;
      if (i >= 8 || source.length > 16384) {
        fallback(box, source, "Show diagram source");
        continue;
      }
      try {
        const {svg} = await api.render("slate-diagram-" + version + "-" + i, lenientSequence(source));
        if (version !== revision) continue;
        box.innerHTML = svg;
        zoomable(box);
      } catch (error) {
        const reason = error && error.message ? String(error.message).split("\n")[0].slice(0, 160) : "";
        if (version === revision) fallback(box, source, "Could not render diagram" + (reason ? ": " + reason : ""));
      }
    }
  }

  function render(markdown, colors = {}) {
    const version = ++revision;
    const dark = colors.dark !== false;
    const theme = dark
      ? {background:"#101010", surface:"#242424", text:"#e7e7e7", accent:"#ccc", muted:"#aaa", border:"#505050", code:"#d2d2d2"}
      : {background:"#fbfbfb", surface:"#eee", text:"#252525", accent:"#444", muted:"#626262", border:"#bbb", code:"#444444"};
    const style = document.documentElement.style;
    style.colorScheme = dark ? "dark" : "light";
    // The app's text size: every rem and em in the stylesheet scales from it.
    const size = colors.fontSize;
    if (typeof size === "number" && size >= 6 && size <= 72) style.fontSize = size + "px";
    else style.removeProperty("font-size");
    for (const key of Object.keys(theme)) {
      if (typeof colors[key] === "string" && /^#[a-f\d]{6}$/i.test(colors[key])) theme[key] = colors[key];
      style.setProperty("--" + key, theme[key]);
    }
    theme.dark = dark;
    drag = gesture = queued = null;
    output.innerHTML = md.render(markdown);
    const anchors = new Map();
    for (const heading of output.querySelectorAll("h1,h2,h3,h4,h5,h6")) {
      const slug = heading.textContent.trim().toLowerCase().replace(/[^\p{L}\p{N}\s_-]/gu, "").replace(/\s+/g, "-");
      const count = anchors.get(slug) || 0;
      anchors.set(slug, count + 1);
      heading.id = "heading-" + slug + (count ? "-" + count : "");
    }
    for (const li of output.querySelectorAll("li")) {
      const text = (li.firstElementChild?.tagName === "P" ? li.firstElementChild : li).firstChild;
      if (text?.nodeType !== Node.TEXT_NODE || !/^\[[ xX]\]\s/.test(text.data)) continue;
      const checkbox = document.createElement("input");
      checkbox.type = "checkbox";
      checkbox.disabled = true;
      checkbox.checked = text.data[1].toLowerCase() === "x";
      text.deleteData(0, 3);
      text.before(checkbox);
    }
    const boxes = Array.from(output.querySelectorAll(".diagram"));
    if (!boxes.length) return Promise.resolve();
    work = work.then(() => diagrams(boxes, theme, version)).catch(() => {
      if (version === revision) for (const box of boxes) fallback(box, box.textContent, "Diagram renderer unavailable");
    });
    return work;
  }
  // Keep document navigation inside the sandboxed preview.
  output.addEventListener("click", event => {
    const control = event.target.closest?.("[data-zoom]");
    if (control) return zoomControl(control);
    const link = event.target.closest?.("a");
    if (!link) return;
    event.preventDefault();
    const href = link.getAttribute("href") || "";
    if (href.startsWith("#")) {
      try { document.getElementById("heading-" + decodeURIComponent(href.slice(1)))?.scrollIntoView(); }
      catch { /* Malformed fragment. */ }
    }
  });
  window.slatePreview = {render};
})();
