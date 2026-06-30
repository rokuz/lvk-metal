struct RowPC {
  uint count;
  float rowY;
};

[[vk::push_constant]] RowPC pc;

struct V2F {
  float4 position : SV_Position;
  float4 color : COLOR;
};

struct Payload {
  uint cell;
};

float3 hue(float h) {
  return clamp(abs(frac(h + float3(0.0, 0.6667, 0.3333)) * 6.0 - 3.0) - 1.0, 0.0, 1.0);
}

V2F placeTriangle(uint vid, uint index, uint count, float rowY) {
  const float2 tri[3] = {float2(0.0, 0.16), float2(-0.11, -0.16), float2(0.11, -0.16)};
  const float x = (float(index) + 0.5) / float(count) * 2.0 - 1.0;
  V2F o;
  o.position = float4(float2(x, rowY) + tri[vid], 0.0, 1.0);
  o.color = float4(hue(float(index) / float(count)), 1.0);
  return o;
}

[shader("amplification")]
[numthreads(1, 1, 1)]
void meshObjectMain(uint3 gid: SV_GroupID) {
  Payload p;
  p.cell = gid.x;
  DispatchMesh(1, 1, 1, p);
}

[shader("mesh")]
[numthreads(1, 1, 1)]
[outputtopology("triangle")]
void meshMeshMain(in payload Payload p, OutputVertices<V2F, 3> verts, OutputIndices<uint3, 1> triangles) {
  SetMeshOutputCounts(3, 1);
  for (uint v = 0; v < 3; v++) {
    verts[v] = placeTriangle(v, p.cell, pc.count, pc.rowY);
  }
  triangles[0] = uint3(0, 1, 2);
}

[shader("fragment")]
float4 meshFragmentMain(V2F input) : SV_Target {
  return input.color;
}
