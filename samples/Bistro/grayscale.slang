import lvk_metal_bindless;

struct PC {
  uint tex;
  uint width;
  uint height;
};

[[vk::push_constant]] PC pc;

[shader("compute")]
[numthreads(16, 16, 1)]
void grayscaleMain(uint3 globalID : SV_DispatchThreadID) {
  if (globalID.x < pc.width && globalID.y < pc.height) {
    const int2 pos = int2(globalID.xy);
    const float4 px = kImages2D.data[pc.tex][pos];
    const float l = dot(px, float4(0.299, 0.587, 0.114, 0.0));
    kImages2D.data[pc.tex][pos] = float4(l, l, l, 1.0);
  }
}
