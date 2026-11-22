"use strict";

const diagram = document.getElementById("diagram");
mermaid.initialize({
  startOnLoad: false,
  securityLevel: "strict",
  htmlLabels: false,
  theme: "default",
  maxTextSize: 16384,
  maxEdges: 200,
  suppressErrorRendering: true
});

async function slateRender(source) {
  diagram.replaceChildren();
  try {
    const { svg } = await mermaid.render("slate-diagram", source, diagram);
    diagram.innerHTML = svg;
    const graphic = diagram.querySelector("svg");
    const bounds = graphic.viewBox.baseVal;
    if (!(bounds.width > 0 && bounds.height > 0)) return "";
    const scale = Math.min(1, 700 / bounds.width, 520 / bounds.height);
    const width = Math.max(1, Math.ceil(bounds.width * scale));
    const height = Math.max(1, Math.ceil(bounds.height * scale));
    graphic.style.width = width + "px";
    graphic.style.height = height + "px";
    const screenshot = (width + 20) + "," + (height + 20);
    // SVG-only diagrams can rasterize directly, without waiting for a new
    // compositor frame. HTML labels still need a browser screenshot.
    if (graphic.querySelector("foreignObject")) return screenshot;
    try {
      const image = new Image();
      const xml = new XMLSerializer().serializeToString(graphic);
      image.src = "data:image/svg+xml;charset=utf-8," + encodeURIComponent(xml);
      await image.decode();
      const canvas = document.createElement("canvas");
      canvas.width = width + 20;
      canvas.height = height + 20;
      const context = canvas.getContext("2d", { alpha: false });
      context.fillStyle = "white";
      context.fillRect(0, 0, canvas.width, canvas.height);
      context.drawImage(image, 10, 10, width, height);
      return canvas.toDataURL("image/png").slice("data:image/png;base64,".length);
    } catch {
      return screenshot;
    }
  } catch {
    diagram.replaceChildren();
    return "";
  }
}
