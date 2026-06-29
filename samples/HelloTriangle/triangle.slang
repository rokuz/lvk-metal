[vk::constant_id(0)]
const int kShape = 0;

struct VertexOut {
  float4 position : SV_Position;
  float4 color : COLOR;
};

static const float2 kTriPos[3] = {float2(0.0, 0.6), float2(-0.6, -0.6), float2(0.6, -0.6)};
static const float3 kTriCol[3] = {float3(1.0, 0.0, 0.0), float3(0.0, 1.0, 0.0), float3(0.0, 0.0, 1.0)};

static const float2 kQuadPos[6] = {float2(-0.6, -0.6), float2(0.6, -0.6), float2(0.6, 0.6),
                                    float2(-0.6, -0.6), float2(0.6, 0.6), float2(-0.6, 0.6)};
static const float3 kQuadCol[6] = {float3(1.0, 0.0, 0.0), float3(0.0, 1.0, 0.0), float3(0.0, 0.0, 1.0),
                                    float3(1.0, 0.0, 0.0), float3(0.0, 0.0, 1.0), float3(1.0, 1.0, 0.0)};

[shader("vertex")]
VertexOut vertexMain(uint vid : SV_VertexID) {
  VertexOut out;
  if (kShape == 1) {
    out.position = float4(kQuadPos[vid], 0.0, 1.0);
    out.color = float4(kQuadCol[vid], 1.0);
  } else {
    out.position = float4(kTriPos[vid], 0.0, 1.0);
    out.color = float4(kTriCol[vid], 1.0);
  }
  return out;
}

[shader("fragment")]
float4 fragmentMain(VertexOut input) : SV_Target {
  return input.color;
}
