import lvk_metal_bindless;

struct Vertex {
  float x, y, z;
  float u, v;
  float nx, ny, nz;
};

struct ShadowVP {
  float4x4 viewProj[6];
};

struct ShadowPC {
  float4 lightPos;
  Vertex* vb;
  ShadowVP* vp;
  float shadowFar;
};

[[vk::push_constant]] ShadowPC pc;

struct VertexStageOutput {
  float4 sv_Position : SV_Position;
  float3 worldPos;
};

[shader("vertex")]
VertexStageOutput vertexMain(uint vertexID : SV_VertexID, uint viewIndex : SV_ViewID) {
  const Vertex v = pc.vb[vertexID];
  VertexStageOutput out;
  out.worldPos = float3(v.x, v.y, v.z);
  out.sv_Position = pc.vp.viewProj[viewIndex] * float4(out.worldPos, 1.0);
  return out;
}

[shader("fragment")]
float fragmentMain(VertexStageOutput input) : SV_Target {
  return length(input.worldPos - pc.lightPos.xyz) / pc.shadowFar;
}
