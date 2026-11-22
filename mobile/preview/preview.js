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

  async function diagrams(boxes, theme, version) {
    if (version !== revision) return;
    const api = await loadDiagrams();
    if (version !== revision) return;
    api.initialize({
      startOnLoad: false, securityLevel: "strict", theme: "base", look: "classic",
      htmlLabels: false, maxTextSize: 16384, maxEdges: 200, suppressErrorRendering: true,
      themeVariables: {
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
        const {svg} = await api.render("slate-diagram-" + version + "-" + i, source);
        if (version === revision) box.innerHTML = svg;
      } catch {
        if (version === revision) fallback(box, source, "Could not render diagram");
      }
    }
  }

  function render(markdown, colors = {}) {
    const version = ++revision;
    const dark = colors.dark !== false;
    const theme = dark
      ? {background:"#101010", surface:"#242424", text:"#e7e7e7", accent:"#ccc", muted:"#aaa", border:"#505050"}
      : {background:"#fbfbfb", surface:"#eee", text:"#252525", accent:"#444", muted:"#626262", border:"#bbb"};
    const style = document.documentElement.style;
    style.colorScheme = dark ? "dark" : "light";
    for (const key of Object.keys(theme)) {
      if (typeof colors[key] === "string" && /^#[a-f\d]{6}$/i.test(colors[key])) theme[key] = colors[key];
      style.setProperty("--" + key, theme[key]);
    }
    theme.dark = dark;
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
