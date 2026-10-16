export const catalog = {
  InputNode: {
    label: "Wejście",
    group: "Wejście / wyjście",
    icon: "input",
    description: "Odczyt pliku multimedialnego",
    defaults: { filename: "input.mp4" },
    in: null,
    out: "packet",
  },
  OutputNode: {
    label: "Wyjście",
    group: "Wejście / wyjście",
    icon: "output",
    description: "Zapis strumieni do pliku",
    defaults: { filename: "output.mp4" },
    in: "packet",
    out: null,
  },
  DemuxNode: {
    label: "Demuxer",
    group: "Routing",
    icon: "split",
    description: "Rozdzielanie strumieni",
    defaults: { ports: "2" },
    in: "packet",
    out: "packet",
  },
  MuxerNode: {
    label: "Muxer",
    group: "Routing",
    icon: "merge",
    description: "Łączenie strumieni",
    defaults: { ports: "2" },
    in: "packet",
    out: "packet",
  },
  VideoDecoderNode: {
    label: "Dekoder wideo",
    group: "Wideo",
    icon: "video",
    description: "Pakiety → klatki wideo",
    defaults: { threads: "1" },
    in: "packet",
    out: "frame",
  },
  VideoEncoderNode: {
    label: "Enkoder wideo",
    group: "Wideo",
    icon: "video",
    description: "Klatki wideo → pakiety",
    defaults: { codec: "mpeg4", bit_rate: "2000000" },
    in: "frame",
    out: "packet",
  },
  VideoScalerNode: {
    label: "Skalowanie wideo",
    group: "Wideo",
    icon: "scale",
    description: "Rozdzielczość i format pikseli",
    defaults: {
      width: "1280",
      height: "720",
      pixel_format: "yuv420p",
      algorithm: "bilinear",
    },
    in: "frame",
    out: "frame",
  },
  VideoOverlayNode: {
    label: "Overlay WWW",
    group: "Wideo",
    icon: "layers",
    description: "Nakładka HTML w czasie rzeczywistym",
    defaults: {
      url: "http://127.0.0.1:3000/overlay.html",
      width: "1920",
      height: "1080",
      fps: "60",
      render_mode: "cpu",
    },
    in: "frame",
    out: "frame",
  },
  AudioDecoderNode: {
    label: "Dekoder audio",
    group: "Audio",
    icon: "audio",
    description: "Pakiety → próbki audio",
    defaults: { threads: "1" },
    in: "packet",
    out: "frame",
  },
  AudioEncoderNode: {
    label: "Enkoder audio",
    group: "Audio",
    icon: "audio",
    description: "Próbki audio → pakiety",
    defaults: { codec: "aac", bit_rate: "128000" },
    in: "frame",
    out: "packet",
  },
  AudioResamplerNode: {
    label: "Resampling audio",
    group: "Audio",
    icon: "audio",
    description: "Częstotliwość i układ kanałów",
    defaults: {
      sample_rate: "48000",
      sample_format: "fltp",
      channel_layout: "stereo",
    },
    in: "frame",
    out: "frame",
  },
};
export const labels = {
  filename: "Ścieżka pliku",
  ports: "Liczba portów",
  threads: "Wątki",
  codec: "Kodek",
  bit_rate: "Bitrate (bit/s)",
  width: "Szerokość",
  height: "Wysokość",
  pixel_format: "Format pikseli",
  algorithm: "Algorytm",
  url: "Adres URL",
  fps: "Klatki / s",
  render_mode: "Tryb renderowania",
  sample_rate: "Częstotliwość (Hz)",
  sample_format: "Format próbek",
  channel_layout: "Układ kanałów",
};
export function ports(node, direction) {
  const spec = catalog[node.data.kind];
  if (!spec?.[direction]) return [];
  const multiple =
    (direction === "in" && node.data.kind === "MuxerNode") ||
    (direction === "out" && node.data.kind === "DemuxNode");
  return Array.from(
    {
      length: multiple
        ? Math.min(64, Math.max(1, Number(node.data.params.ports) || 1))
        : 1,
    },
    (_, i) => i,
  );
}
export function canConnect(connection, nodes, edges) {
  const s = nodes.find((n) => n.id === connection.source),
    t = nodes.find((n) => n.id === connection.target);
  if (
    !s ||
    !t ||
    s === t ||
    catalog[s.data.kind].out !== catalog[t.data.kind].in
  )
    return false;
  if (
    catalog[s.data.kind].out === "frame" &&
    catalog[s.data.kind].group !== catalog[t.data.kind].group
  )
    return false;
  if (
    !ports(s, "out").includes(Number(connection.sourceHandle)) ||
    !ports(t, "in").includes(Number(connection.targetHandle))
  )
    return false;
  if (
    edges.some(
      (e) =>
        (e.source === s.id && e.sourceHandle === connection.sourceHandle) ||
        (e.target === t.id && e.targetHandle === connection.targetHandle),
    )
  )
    return false;
  // Workers use blocking queues: do not permit directed cycles.
  const seen = new Set();
  function reaches(id) {
    if (id === s.id) return true;
    if (seen.has(id)) return false;
    seen.add(id);
    return edges.filter((e) => e.source === id).some((e) => reaches(e.target));
  }
  return !reaches(t.id);
}
export function decode(document) {
  const graph = document.configuration.graph;
  const nodes = graph.map((n, i) => ({
    id: n.name,
    type: "media",
    position: document.editor?.positions?.[n.name] || {
      x: (i % 4) * 285,
      y: Math.floor(i / 4) * 200,
    },
    data: {
      kind: n.type,
      name: n.name,
      params: Object.assign({}, ...n.configuration),
    },
  }));
  const edges = graph.flatMap((n) =>
    n.connections.map((c, i) => ({
      id: `${n.name}-${i}`,
      source: n.name,
      target: typeof c === "string" ? c : c.node,
      sourceHandle: String(typeof c === "string" ? i : (c.output_port ?? i)),
      targetHandle: String(typeof c === "string" ? 0 : (c.input_port ?? 0)),
      type: "smoothstep",
    })),
  );
  return { nodes, edges };
}
export function encode(name, nodes, edges, base = {}) {
  return {
    name,
    configuration: {
      ...base,
      graph: nodes.map((n) => ({
        name: n.id,
        type: n.data.kind,
        configuration: [
          Object.fromEntries(
            Object.entries(n.data.params).filter(([, v]) => v !== ""),
          ),
        ],
        connections: edges
          .filter((e) => e.source === n.id)
          .map((e) => ({
            node: e.target,
            output_port: Number(e.sourceHandle),
            input_port: Number(e.targetHandle),
          })),
      })),
    },
    editor: {
      positions: Object.fromEntries(nodes.map((n) => [n.id, n.position])),
    },
  };
}
export function validate(nodes, edges) {
  if (!nodes.length) return "Dodaj przynajmniej jeden element do grafu.";
  for (const n of nodes) {
    if (!catalog[n.data.kind]) return "Nieznany typ elementu.";
    if (
      ["InputNode", "OutputNode"].includes(n.data.kind) &&
      !n.data.params.filename?.trim()
    )
      return `${n.id}: podaj ścieżkę pliku.`;
    if (
      ["DemuxNode", "MuxerNode"].includes(n.data.kind) &&
      (!/^\d+$/.test(n.data.params.ports) ||
        +n.data.params.ports < 1 ||
        +n.data.params.ports > 64)
    )
      return `${n.id}: liczba portów musi mieścić się w zakresie 1–64.`;
  }
  for (const e of edges)
    if (
      !canConnect(
        e,
        nodes,
        edges.filter((x) => x !== e),
      )
    )
      return "Graf zawiera niezgodne połączenie, zajęty port lub cykl.";
  return null;
}
export const initialDocument = {
  name: "Nowy graf",
  configuration: { graph: [] },
  editor: { positions: {} },
};
export const remuxDocument = {
  name: "Remuksowanie MP4",
  configuration: {
    graph: [
      {
        name: "input",
        type: "InputNode",
        configuration: [{ filename: "input.mp4" }],
        connections: ["output"],
      },
      {
        name: "output",
        type: "OutputNode",
        configuration: [{ filename: "output.mp4" }],
        connections: [],
      },
    ],
  },
  editor: {
    positions: { input: { x: 80, y: 160 }, output: { x: 450, y: 160 } },
  },
};
