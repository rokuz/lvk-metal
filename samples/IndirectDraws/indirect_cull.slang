struct DrawArgs {
  uint vertexCount;
  uint instanceCount;
  uint vertexStart;
  uint baseInstance;
};

struct CullPC {
  DrawArgs* cmds;
  uint* primTypes;
  uint count;
};

[[vk::push_constant]] CullPC pc;

[shader("compute")]
[numthreads(64, 1, 1)]
void fillCommands(uint3 tid: SV_DispatchThreadID) {
  const uint i = tid.x;
  if (i >= pc.count) {
    return;
  }
  DrawArgs a;
  a.vertexCount = 3;
  a.instanceCount = 1;
  a.vertexStart = 0;
  a.baseInstance = i;
  pc.cmds[i] = a;
  pc.primTypes[i] = (i & 1u) != 0u ? 2u : 3u;
}
