import test from "node:test";
import assert from "node:assert/strict";
import {
  decode,
  encode,
  canConnect,
  validate,
  remuxDocument,
  catalog,
} from "./graph.js";
test("configuration round trip preserves ports, optional settings and positions", () => {
  const document = {
    name: "test",
    configuration: {
      queue_capacity: 8,
      graph: [
        {
          name: "a",
          type: "InputNode",
          configuration: [{ filename: "a.mp4" }],
          connections: [{ node: "mux", input_port: 1, output_port: 0 }],
        },
        {
          name: "mux",
          type: "MuxerNode",
          configuration: [{ ports: "2" }],
          connections: [],
        },
      ],
    },
    editor: { positions: { a: { x: 12, y: 33 } } },
  };
  const { nodes, edges } = decode(document);
  const result = encode(document.name, nodes, edges, document.configuration);
  assert.equal(result.configuration.queue_capacity, 8);
  assert.deepEqual(result.configuration.graph, document.configuration.graph);
  assert.deepEqual(result.editor.positions.a, { x: 12, y: 33 });
  assert.equal(validate(nodes, edges), null);
});
test("connections reject occupied ports, incompatible types, self loops and cycles", () => {
  const n = (id, kind) => ({
    id,
    data: { kind, params: catalog[kind].defaults },
  });
  const nodes = [
    n("a", "VideoScalerNode"),
    n("b", "VideoScalerNode"),
    n("c", "OutputNode"),
    n("d", "VideoScalerNode"),
  ];
  const edge = (source, target) => ({
    source,
    target,
    sourceHandle: "0",
    targetHandle: "0",
  });
  assert.equal(canConnect(edge("a", "b"), nodes, []), true);
  assert.equal(canConnect(edge("a", "c"), nodes, []), false);
  assert.equal(canConnect(edge("a", "a"), nodes, []), false);
  assert.equal(canConnect(edge("b", "a"), nodes, [edge("a", "b")]), false);
  assert.equal(canConnect(edge("a", "d"), nodes, [edge("a", "b")]), false);
  assert.equal(canConnect(edge("d", "b"), nodes, [edge("a", "b")]), false);
});
test("decreasing port count invalidates old connections", () => {
  const { nodes, edges } = decode(remuxDocument);
  nodes[0].data.kind = "DemuxNode";
  nodes[0].data.params = { ports: "1" };
  edges[0].sourceHandle = "1";
  assert.match(validate(nodes, edges), /połączenie/);
});
test("empty graphs and missing filenames cannot be started", () => {
  assert.ok(validate([], []));
  const { nodes, edges } = decode(remuxDocument);
  nodes[0].data.params.filename = "";
  assert.match(validate(nodes, edges), /ścieżkę/);
});
test("audio frames cannot connect to video consumers", () => {
  const nodes = [
    { id: "audio", data: { kind: "AudioDecoderNode", params: {} } },
    { id: "video", data: { kind: "VideoEncoderNode", params: {} } },
  ];
  assert.equal(
    canConnect(
      {
        source: "audio",
        target: "video",
        sourceHandle: "0",
        targetHandle: "0",
      },
      nodes,
      [],
    ),
    false,
  );
});
test("RTMP sink validates URL and preserves options in graph conversion", () => {
  const document = {
    name: "RTMP",
    configuration: { graph: [
      { name: "source", type: "InputNode", configuration: [{ filename: "input.mp4" }], connections: ["sink"] },
      { name: "sink", type: "RTMPOutputNode", configuration: [{ url: "rtmp://localhost/live/test", realtime: "false" }], connections: [] },
    ] },
  };
  const { nodes, edges } = decode(document);
  assert.equal(validate(nodes, edges), null);
  assert.deepEqual(encode("RTMP", nodes, edges).configuration.graph, document.configuration.graph.map((n, i) => ({
    ...n,
    connections: i === 0 ? [{ node: "sink", output_port: 0, input_port: 0 }] : [],
  })));
  nodes[1].data.params.url = "http://localhost/live/test";
  assert.match(validate(nodes, edges), /rtmp:\/\//);
});
