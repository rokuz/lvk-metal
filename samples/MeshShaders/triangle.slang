struct VertexOutput {
  float4 sv_Position : SV_Position;
  float3 color : COLOR;
};

struct MeshPayload {
  uint unused;
};

[shader("amplification")]
[numthreads(1, 1, 1)]
void taskMain() {
  MeshPayload p;
  DispatchMesh(1, 1, 1, p);
}

[shader("mesh")]
[numthreads(1, 1, 1)]
[outputtopology("triangle")]
void meshMain(in payload MeshPayload meshPayload, OutputVertices<VertexOutput, 3> verts, OutputIndices<uint3, 1> triangles) {
  const float2 pos[3] = {float2(-0.6, -0.4), float2(0.6, -0.4), float2(0.0, 0.6)};
  const float3 col[3] = {float3(1.0, 0.0, 0.0), float3(0.0, 1.0, 0.0), float3(0.0, 0.0, 1.0)};

  SetMeshOutputCounts(3, 1);

  for (uint i = 0; i < 3; i++) {
    VertexOutput v;
    v.sv_Position = float4(pos[i], 0.0, 1.0);
    v.color = col[i];
    verts[i] = v;
  }

  triangles[0] = uint3(0, 1, 2);
}

[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target {
  return float4(input.color, 1.0);
}
