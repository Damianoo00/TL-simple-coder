import React, { useCallback, useEffect, useRef, useState } from "react";
import {
  ReactFlow,
  Background,
  Controls,
  MiniMap,
  Handle,
  Position,
  applyNodeChanges,
  applyEdgeChanges,
  addEdge,
  useReactFlow,
  useUpdateNodeInternals,
} from "@xyflow/react";
import {
  Activity,
  AudioLines,
  Check,
  ChevronDown,
  ChevronRight,
  Circle,
  FolderOpen,
  GitBranch,
  GripVertical,
  Layers,
  Maximize2,
  Merge,
  MonitorPlay,
  Play,
  Plus,
  Save,
  Scaling,
  Search,
  Split,
  Square,
  Terminal,
  Trash2,
  Video,
  X,
  ArrowRightToLine,
  ArrowLeftFromLine,
  Download,
  Upload,
  Settings2,
} from "lucide-react";
import {
  catalog,
  labels,
  ports,
  canConnect,
  decode,
  encode,
  validate,
  initialDocument,
  remuxDocument,
} from "./graph.js";
const icons = {
  input: ArrowRightToLine,
  output: ArrowLeftFromLine,
  video: Video,
  audio: AudioLines,
  split: Split,
  merge: Merge,
  scale: Scaling,
  layers: Layers,
};
const statusLabels = {
  running: "Uruchomiony",
  starting: "Uruchamianie",
  stopped: "Zatrzymany",
  failed: "Błąd",
};
const active = (r) => r && ["running", "starting"].includes(r.status);
async function api(path, method = "GET", body) {
  const response = await fetch(path, {
    method,
    headers: body ? { "Content-Type": "application/json" } : undefined,
    body: body ? JSON.stringify(body) : undefined,
  });
  const data = await response.json();
  if (!response.ok)
    throw new Error(data.error || `Błąd HTTP ${response.status}`);
  return data;
}
function MediaNode({ id, data, selected }) {
  const spec = catalog[data.kind],
    Icon = icons[spec.icon],
    update = useUpdateNodeInternals();
  useEffect(() => {
    update(id);
  }, [id, data.params.ports, update]);
  const inputs = ports({ data }, "in"),
    outputs = ports({ data }, "out");
  return (
    <div
      className={`media-node ${selected ? "chosen" : ""} ${spec.group === "Audio" ? "audio" : ""}`}
    >
      <div className="node-top">
        <span className="node-icon">
          <Icon size={17} />
        </span>
        <span>{spec.label}</span>
        <span className="node-menu">···</span>
      </div>
      <div className="node-id">{data.name}</div>
      <div className="node-description">
        {data.params.filename ||
          data.params.codec ||
          (data.kind === "VideoScalerNode"
            ? `${data.params.width} × ${data.params.height}`
            : spec.description)}
      </div>
      <div className="node-ports">
        {[0, 1].map((side) => (
          <div key={side}>
            {(side ? outputs : inputs).map((i) => (
              <div className="port-row" key={i}>
                <Handle
                  type={side ? "source" : "target"}
                  position={side ? Position.Right : Position.Left}
                  id={String(i)}
                  style={{
                    top: "50%",
                    background:
                      spec[side ? "out" : "in"] === "frame"
                        ? "#b69bea"
                        : "#1db8cc",
                  }}
                />
                <span>
                  {spec[side ? "out" : "in"] === "frame" ? "ramki" : "pakiety"}
                  {(side ? outputs : inputs).length > 1 ? ` ${i}` : ""}
                </span>
              </div>
            ))}
          </div>
        ))}
      </div>
    </div>
  );
}
const nodeTypes = { media: MediaNode };
export default function App() {
  const [library, setLibrary] = useState([]),
    [runs, setRuns] = useState([]),
    [connected, setConnected] = useState(false);
  const [docId, setDocId] = useState(null),
    [name, setName] = useState("Nowy graf"),
    [nodes, setNodes] = useState([]),
    [edges, setEdges] = useState([]),
    [base, setBase] = useState({});
  const [dirty, setDirty] = useState(false),
    [selected, setSelected] = useState(null),
    [search, setSearch] = useState(""),
    [graphSearch, setGraphSearch] = useState("");
  const [busy, setBusy] = useState(false),
    [error, setError] = useState(""),
    [notice, setNotice] = useState(""),
    [logs, setLogs] = useState(""),
    [logOffset, setLogOffset] = useState(0),
    [filter, setFilter] = useState("all"),
    [follow, setFollow] = useState(true),
    [logHeight, setLogHeight] = useState(205);
  const [cleared, setCleared] = useState({}),
    [revision, setRevision] = useState(false);
  const drafts = useRef(new Map()),
    logRef = useRef(null),
    fileRef = useRef(null),
    sequence = useRef(0);
  const flow = useReactFlow();
  const run = runs.filter((r) => r.document_id === docId).at(-1),
    running = active(run),
    node = nodes.find((n) => n.id === selected);
  const refresh = useCallback(async () => {
    const [l, r] = await Promise.all([api("/library"), api("/graphs")]);
    setLibrary(l);
    setRuns(r);
    setConnected(true);
    return l;
  }, []);
  useEffect(() => {
    let live = true;
    const poll = async () => {
      try {
        const [l, r] = await Promise.all([api("/library"), api("/graphs")]);
        if (live) {
          setLibrary(l);
          setRuns(r);
          setConnected(true);
        }
      } catch {
        if (live) setConnected(false);
      }
    };
    poll();
    const id = setInterval(poll, 1500);
    return () => {
      live = false;
      clearInterval(id);
    };
  }, []);
  useEffect(() => {
    let live = true;
    setLogs("");
    setLogOffset(0);
    if (!run) return;
    const poll = async () => {
      try {
        const value = await api(
          `/graphs/${run.graph_id}/logs?since=${cleared[run.graph_id] || 0}`,
        );
        if (live) {
          setLogs(value.text);
          setLogOffset(value.offset || 0);
        }
      } catch (e) {
        if (live) setError(e.message);
      }
    };
    poll();
    const id = setInterval(poll, 1000);
    return () => {
      live = false;
      clearInterval(id);
    };
  }, [run?.graph_id, cleared]);
  useEffect(() => {
    if (follow && logRef.current)
      logRef.current.scrollTop = logRef.current.scrollHeight;
  }, [logs, follow]);
  useEffect(() => {
    const warn = (e) => {
      if (dirty || [...drafts.current.values()].some((d) => d.dirty)) {
        e.preventDefault();
        e.returnValue = "";
      }
    };
    window.addEventListener("beforeunload", warn);
    return () => window.removeEventListener("beforeunload", warn);
  }, [dirty]);
  const changed = () => {
    setDirty(true);
    if (running) setRevision(true);
  };
  function remember() {
    drafts.current.set(docId ?? "new", {
      name,
      nodes,
      edges,
      base,
      dirty,
      revision,
    });
  }
  function open(document) {
    remember();
    const draft = drafts.current.get(document.id ?? "new");
    const graph = draft || {
      ...decode(document),
      name: document.name,
      base: document.configuration,
      dirty: false,
      revision: false,
    };
    setDocId(document.id ?? null);
    setName(graph.name);
    setNodes(graph.nodes);
    setEdges(graph.edges);
    setBase(graph.base);
    setDirty(graph.dirty);
    setRevision(graph.revision);
    setSelected(null);
    setError("");
    setNotice("");
    setTimeout(() => flow.fitView({ padding: 0.25, duration: 250, maxZoom: 1 }), 70);
  }
  function fresh(template = initialDocument) {
    if (
      docId === null &&
      dirty &&
      nodes.length &&
      !window.confirm(
        "Zastąpić niezapisany graf nowym dokumentem? Niezapisane zmiany zostaną utracone.",
      )
    )
      return;
    remember();
    drafts.current.delete("new");
    const graph = decode(template);
    setDocId(null);
    setName(template.name);
    setNodes(graph.nodes);
    setEdges(graph.edges);
    setBase(template.configuration);
    setDirty(true);
    setRevision(false);
    setSelected(null);
    setNotice("");
    setError("");
    setTimeout(() => flow.fitView({ padding: 0.3, maxZoom: 1 }), 70);
  }
  async function action(fn) {
    setBusy(true);
    setError("");
    setNotice("");
    try {
      await fn();
    } catch (e) {
      setError(e.message);
    } finally {
      setBusy(false);
    }
  }
  async function save() {
    if (!name.trim()) throw new Error("Podaj nazwę grafu.");
    const problem = validate(nodes, edges);
    if (problem) throw new Error(problem);
    const body = encode(name.trim(), nodes, edges, base);
    const result = await api(
      docId ? `/library/${docId}` : "/library",
      docId ? "PUT" : "POST",
      body,
    );
    drafts.current.delete(docId ?? "new");
    setDocId(result.id);
    setDirty(false);
    await refresh();
    return result;
  }
  function add(kind, position) {
    let id;
    do {
      id = `${kind.replace("Node", "").toLowerCase()}_${++sequence.current}`;
    } while (nodes.some((n) => n.id === id));
    setNodes((v) => [
      ...v,
      {
        id,
        type: "media",
        position:
          position ||
          flow.screenToFlowPosition({
            x: window.innerWidth / 2 - 100,
            y: window.innerHeight / 2 - 80,
          }),
        data: { kind, name: id, params: { ...catalog[kind].defaults } },
      },
    ]);
    setSelected(id);
    changed();
  }
  function updateParam(key, value) {
    setNodes((v) =>
      v.map((n) =>
        n.id === selected
          ? {
              ...n,
              data: { ...n.data, params: { ...n.data.params, [key]: value } },
            }
          : n,
      ),
    );
    changed();
  }
  function removeNode() {
    setNodes((v) => v.filter((n) => n.id !== selected));
    setEdges((v) =>
      v.filter((e) => e.source !== selected && e.target !== selected),
    );
    setSelected(null);
    changed();
  }
  async function start() {
    const saved = await save();
    await api(`/library/${saved.id}/start`, "POST");
    setRevision(false);
    await refresh();
    setNotice("Wysłano żądanie uruchomienia grafu.");
  }
  function exportGraph() {
    const url = URL.createObjectURL(
      new Blob([JSON.stringify(encode(name, nodes, edges, base), null, 2)], {
        type: "application/json",
      }),
    );
    const link = document.createElement("a");
    link.href = url;
    link.download = `${name.replace(/[^\p{L}\p{N}_-]/gu, "_")}.json`;
    link.click();
    URL.revokeObjectURL(url);
  }
  async function importGraph(event) {
    const file = event.target.files[0];
    event.target.value = "";
    if (!file) return;
    await action(async () => {
      const document = JSON.parse(await file.text());
      const configuration = document.configuration || document;
      if (!Array.isArray(configuration.graph))
        throw new Error("Plik nie zawiera tablicy graph.");
      for (const n of configuration.graph) {
        if (
          !catalog[n.type] ||
          !n.name ||
          !Array.isArray(n.connections) ||
          !Array.isArray(n.configuration)
        )
          throw new Error("Nieprawidłowy format noda.");
      }
      if (
        new Set(configuration.graph.map((n) => n.name)).size !==
        configuration.graph.length
      )
        throw new Error("Nazwy nodów muszą być unikalne.");
      const normalized = {
        ...document,
        name: document.name || file.name.replace(/\.json$/, ""),
        configuration,
      };
      const graph = decode(normalized),
        problem = validate(graph.nodes, graph.edges);
      if (problem) throw new Error(problem);
      fresh(normalized);
    });
  }
  const visibleLogs = logs
    .split("\n")
    .filter(Boolean)
    .filter(
      (line) =>
        filter === "all" ||
        (filter === "errors"
          ? /error|failed|fail|błąd|exception/i.test(line)
          : !/error|failed|fail|błąd|exception/i.test(line)),
    );
  return (
    <div className="studio">
      <header className="app-header">
        <a className="brand" href="/" aria-label="Trybuna Live Simple Coder">
          <span className="brand-mark">
            <GitBranch size={22} />
          </span>
          <span>
            TL<span className="brand-dot">-</span>simple-coder
            <small>Trybuna Live Simple Coder</small>
          </span>
        </a>
        <div className="header-divider" />
        <span className="project-label">
          WORKSPACE <ChevronRight size={12} />
        </span>
        <input
          className="graph-title"
          aria-label="Nazwa grafu"
          value={name}
          disabled={busy}
          onChange={(e) => {
            setName(e.target.value);
            changed();
          }}
        />
        <span className="save-state">
          {dirty ? (
            <>
              <Circle size={6} fill="currentColor" /> Niezapisane zmiany
            </>
          ) : docId ? (
            <>
              <Check size={13} /> Zapisano na dysku
            </>
          ) : (
            "Nowy dokument"
          )}
        </span>
        <div className="header-actions">
          <button
            disabled={busy}
            onClick={() =>
              action(async () => {
                await save();
                setNotice("Graf zapisany na dysku.");
              })
            }
          >
            <Save size={15} /> Zapisz
          </button>
          <span className={`run-status ${run?.status || ""}`}>
            <i />
            {run ? statusLabels[run.status] : "Nie uruchomiono"}
          </span>
          <button
            className="primary"
            disabled={busy || running || !connected}
            onClick={() => action(start)}
          >
            <Play size={14} fill="currentColor" /> Start
          </button>
          <button
            className="stop"
            disabled={busy || !running}
            onClick={() =>
              action(async () => {
                await api(`/graphs/${run.graph_id}`, "DELETE");
                await refresh();
              })
            }
          >
            <Square size={12} fill="currentColor" /> Stop
          </button>
        </div>
      </header>
      <div className="main-layout">
        <aside className="library">
          <div className="panel-heading">
            <span>
              <FolderOpen size={15} /> Twoje grafy
            </span>
            <span className="count">{library.length}</span>
          </div>
          <button className="new-graph" disabled={busy} onClick={() => fresh()}>
            <Plus size={16} /> Nowy graf
          </button>
          <div className="search">
            <Search size={14} />
            <input
              aria-label="Szukaj grafów"
              placeholder="Szukaj grafu…"
              value={graphSearch}
              onChange={(e) => setGraphSearch(e.target.value)}
            />
          </div>
          <div className="library-list">
            {library
              .filter((d) =>
                d.name.toLowerCase().includes(graphSearch.toLowerCase()),
              )
              .map((d) => {
                const r = runs.filter((r) => r.document_id === d.id).at(-1);
                return (
                  <button
                    disabled={busy}
                    key={d.id}
                    className={`graph-item ${docId === d.id ? "current" : ""}`}
                    onClick={() => open(d)}
                  >
                    <span className="graph-item-icon">
                      <GitBranch size={17} />
                    </span>
                    <span className="graph-item-text">
                      <strong>
                        {d.name}
                        {(
                          docId === d.id
                            ? dirty
                            : drafts.current.get(d.id)?.dirty
                        )
                          ? " *"
                          : ""}
                      </strong>
                      <small className={r?.status}>
                        {r ? statusLabels[r.status] : "Gotowy do edycji"}
                      </small>
                    </span>
                    {active(r) && <i className="live-dot" />}
                  </button>
                );
              })}
            {!library.length && (
              <div className="library-empty">
                <FolderOpen size={25} />
                <p>Tu znajdziesz zapisane grafy.</p>
                <small>Utwórz graf lub zaimportuj konfigurację JSON.</small>
              </div>
            )}
          </div>
          <div className="library-bottom">
            <span className="eyebrow">SZYBKI START</span>
            <button disabled={busy} onClick={() => fresh(remuxDocument)}>
              <MonitorPlay size={16} />
              <span>
                Remuksowanie MP4<small>Gotowy graf wejście → wyjście</small>
              </span>
              <Plus size={14} />
            </button>
            <button disabled={busy} onClick={() => fileRef.current.click()}>
              <Upload size={15} /> Importuj JSON
            </button>
            <input
              hidden
              ref={fileRef}
              type="file"
              accept=".json,application/json"
              onChange={importGraph}
            />
          </div>
          <div className="connection">
            <i className={connected ? "online" : ""} />
            {connected ? "Supervisor połączony" : "Brak połączenia z API"}
            <span>LOCAL</span>
          </div>
        </aside>
        <main className="center">
          <div className="canvas-toolbar">
            <span>
              <GitBranch size={14} /> Edytor grafu{" "}
              <span className="toolbar-slash">/</span>
              <b>{nodes.length}</b> elementów{" "}
              <span className="toolbar-slash">·</span> {edges.length} połączeń
            </span>
            <div>
              <button
                title="Eksportuj JSON"
                aria-label="Eksportuj JSON"
                onClick={exportGraph}
              >
                <Download size={15} />
              </button>
              <button
                title="Dopasuj widok"
                aria-label="Dopasuj widok"
                onClick={() => flow.fitView({ padding: 0.2, duration: 250, maxZoom: 1 })}
              >
                <Maximize2 size={15} />
              </button>
            </div>
          </div>
          {(error || notice || revision) && (
            <div
              role={error ? "alert" : "status"}
              className={`message ${error ? "error" : ""}`}
            >
              {error ||
                notice ||
                (revision
                  ? "Zmiany wymagają ponownego uruchomienia grafu."
                  : "")}
              <button
                aria-label="Zamknij komunikat"
                onClick={() => {
                  setError("");
                  setNotice("");
                  setRevision(false);
                }}
              >
                <X size={14} />
              </button>
            </div>
          )}
          <section
            className={`canvas ${busy ? "busy" : ""}`}
            aria-label="Workspace grafu"
            onDragOver={(e) => {
              e.preventDefault();
              e.dataTransfer.dropEffect = "move";
            }}
            onDrop={(e) => {
              e.preventDefault();
              const kind = e.dataTransfer.getData("application/async-node");
              if (catalog[kind])
                add(
                  kind,
                  flow.screenToFlowPosition({ x: e.clientX, y: e.clientY }),
                );
            }}
          >
            <ReactFlow
              nodes={nodes}
              edges={edges}
              nodeTypes={nodeTypes}
              colorMode="dark"
              onNodesChange={(changes) => {
                setNodes((ns) => applyNodeChanges(changes, ns));
                if (
                  changes.some(
                    (c) => c.type !== "select" && c.type !== "dimensions",
                  )
                )
                  changed();
              }}
              onEdgesChange={(changes) => {
                setEdges((es) => applyEdgeChanges(changes, es));
                if (changes.some((c) => c.type !== "select")) changed();
              }}
              onConnect={(connection) => {
                if (canConnect(connection, nodes, edges)) {
                  setEdges((es) =>
                    addEdge({ ...connection, type: "smoothstep" }, es),
                  );
                  changed();
                }
              }}
              isValidConnection={(c) => canConnect(c, nodes, edges)}
              onNodeClick={(_, n) => setSelected(n.id)}
              onPaneClick={() => setSelected(null)}
              fitView
              fitViewOptions={{ maxZoom: 1 }}
              minZoom={0.15}
              maxZoom={2}
              deleteKeyCode={["Backspace", "Delete"]}
              defaultEdgeOptions={{
                style: { stroke: "#1db8cc", strokeWidth: 1.7 },
              }}
            >
              <Background color="#244049" gap={24} size={1} />
              <Controls showInteractive={false} />
              <MiniMap
                style={{ width: 125, height: 80 }}
                nodeColor={(n) =>
                  catalog[n.data.kind]?.group === "Audio"
                    ? "#b69bea"
                    : "#1db8cc"
                }
                maskColor="rgba(6,13,16,.75)"
              />
            </ReactFlow>
            {!nodes.length && (
              <div className="canvas-empty">
                <div className="empty-symbol">
                  <GitBranch size={34} />
                </div>
                <span className="eyebrow">TWÓJ POMYSŁ. TWÓJ PRZEPŁYW.</span>
                <h1>Zbuduj swój pierwszy graf</h1>
                <p>
                  Przeciągnij element z palety po prawej,
                  <br />
                  połącz porty i nadaj danym kierunek.
                </p>
                <button disabled={busy} onClick={() => fresh(remuxDocument)}>
                  <Plus size={15} /> Zacznij od przykładu
                </button>
              </div>
            )}
            <div className="canvas-help">
              Przeciągnij, aby przesunąć <span>·</span> Scroll, aby przybliżyć{" "}
              <span>·</span> Delete, aby usunąć
            </div>
          </section>
          <div
            className="log-resizer"
            role="separator"
            aria-label="Wysokość panelu logów"
            aria-orientation="horizontal"
            tabIndex={0}
            onKeyDown={(e) => {
              if (e.key === "ArrowUp" || e.key === "ArrowDown") {
                e.preventDefault();
                setLogHeight((h) =>
                  Math.min(
                    500,
                    Math.max(100, h + (e.key === "ArrowUp" ? 20 : -20)),
                  ),
                );
              }
            }}
            onPointerDown={(e) => {
              e.currentTarget.setPointerCapture(e.pointerId);
              e.currentTarget.dataset.y = e.clientY;
              e.currentTarget.dataset.height = logHeight;
            }}
            onPointerMove={(e) => {
              if (e.currentTarget.hasPointerCapture(e.pointerId))
                setLogHeight(
                  Math.min(
                    500,
                    Math.max(
                      100,
                      +e.currentTarget.dataset.height +
                        +e.currentTarget.dataset.y -
                        e.clientY,
                    ),
                  ),
                );
            }}
          />
          <section className="logs" style={{ height: logHeight }}>
            <div className="logs-toolbar">
              <span className="logs-title">
                <Terminal size={15} /> Logi{" "}
                <span className="count">{visibleLogs.length}</span>
              </span>
              <span className="log-graph">
                {run ? `${name} · #${run.graph_id}` : "Wybrany graf"}
              </span>
              <select
                aria-label="Filtr logów"
                value={filter}
                onChange={(e) => setFilter(e.target.value)}
              >
                <option value="all">Wszystkie poziomy</option>
                <option value="errors">Błędy</option>
                <option value="info">Informacje</option>
              </select>
              <label className="follow">
                <input
                  type="checkbox"
                  checked={follow}
                  onChange={(e) => setFollow(e.target.checked)}
                />{" "}
                Śledź
              </label>
              <button
                title="Wyczyść widok logów"
                aria-label="Wyczyść widok logów"
                onClick={() =>
                  setCleared((v) => ({ ...v, [run?.graph_id]: logOffset }))
                }
              >
                <Trash2 size={14} />
              </button>
            </div>
            <div
              ref={logRef}
              className="log-content"
              role="log"
              aria-label="Logi workera"
            >
              {visibleLogs.length ? (
                visibleLogs.map((line, i) => (
                  <div
                    className={`log-line ${/error|failed|fail|exception/i.test(line) ? "log-error" : ""}`}
                    key={i}
                  >
                    <span>{String(i + 1).padStart(3, "0")}</span>
                    <pre>{line}</pre>
                  </div>
                ))
              ) : (
                <div className="logs-empty">
                  <Terminal size={20} />
                  <span>
                    {run
                      ? "Brak wpisów dla wybranego filtra."
                      : "Uruchom graf, aby zobaczyć logi przetwarzania."}
                  </span>
                </div>
              )}
            </div>
          </section>
        </main>
        <aside className="right-panel">
          <div className="palette">
            <div className="panel-heading">
              <span>
                <Layers size={15} /> Paleta elementów
              </span>
              <span className="count">{Object.keys(catalog).length}</span>
            </div>
            <div className="search">
              <Search size={14} />
              <input
                aria-label="Szukaj elementów"
                placeholder="Szukaj elementu…"
                value={search}
                onChange={(e) => setSearch(e.target.value)}
              />
            </div>
            <div className="palette-scroll">
              {["Wejście / wyjście", "Wideo", "Audio", "Routing"].map(
                (group) => {
                  const items = Object.entries(catalog).filter(
                    ([kind, s]) =>
                      s.group === group &&
                      `${s.label} ${kind}`
                        .toLowerCase()
                        .includes(search.toLowerCase()),
                  );
                  return items.length ? (
                    <div className="palette-group" key={group}>
                      <h2>
                        <ChevronDown size={12} />
                        {group}
                        <span>{items.length}</span>
                      </h2>
                      {items.map(([kind, s]) => {
                        const Icon = icons[s.icon];
                        return (
                          <button
                            disabled={busy}
                            className={`palette-item ${group === "Audio" ? "audio" : ""}`}
                            draggable
                            onDragStart={(e) => {
                              e.dataTransfer.setData(
                                "application/async-node",
                                kind,
                              );
                              e.dataTransfer.effectAllowed = "move";
                            }}
                            onClick={() => add(kind)}
                            key={kind}
                            title="Przeciągnij na graf lub kliknij, aby dodać"
                          >
                            <span className="palette-icon">
                              <Icon size={16} />
                            </span>
                            <span>
                              <strong>{s.label}</strong>
                              <small>{s.description}</small>
                            </span>
                            <GripVertical size={14} />
                          </button>
                        );
                      })}
                    </div>
                  ) : null;
                },
              )}
            </div>
            <div className="palette-tip">
              Przeciągnij na workspace lub kliknij, aby dodać.
            </div>
          </div>
          <section className="inspector">
            <div className="panel-heading">
              <span>
                <Settings2 size={15} /> Właściwości
              </span>
              {node && (
                <button
                  disabled={busy}
                  title="Usuń element"
                  aria-label="Usuń element"
                  onClick={removeNode}
                >
                  <Trash2 size={14} />
                </button>
              )}
            </div>
            {node ? (
              <fieldset disabled={busy}>
                <div className="inspector-type">{node.data.kind}</div>
                <label>
                  ID elementu
                  <input value={node.id} readOnly />
                </label>
                {Object.keys({
                  ...catalog[node.data.kind].defaults,
                  ...node.data.params,
                }).map((key) => (
                  <label key={key}>
                    {labels[key] || key}
                    <input
                      value={node.data.params[key] ?? ""}
                      onChange={(e) => updateParam(key, e.target.value)}
                    />
                  </label>
                ))}
                <p className="inspector-hint">
                  Konfiguracja zostanie zastosowana przy następnym uruchomieniu.
                </p>
              </fieldset>
            ) : (
              <div className="inspector-empty">
                <Settings2 size={23} />
                <p>
                  Zaznacz element na grafie,
                  <br />
                  aby zmienić jego parametry.
                </p>
              </div>
            )}
          </section>
        </aside>
      </div>
      <footer className="statusbar">
        <span>
          <Activity size={12} /> TRYBUNA LIVE <b>SIMPLE CODER</b>
        </span>
        <span>
          <i className="packet-dot" /> Pakiety <i className="frame-dot" /> Ramki
        </span>
        <span>
          {busy ? "Zapisywanie / sterowanie…" : "Gotowy"}
          <span className="status-separator">|</span> C++ ENGINE
        </span>
      </footer>
    </div>
  );
}
