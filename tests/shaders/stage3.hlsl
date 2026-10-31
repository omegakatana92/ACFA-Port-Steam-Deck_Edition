// Stage 3 state-matrix shader: one quad per draw record, selected by firstInstance.
StructuredBuffer<float4> g_draws : register(t0);  // rect (x0,y0,x1,y1), colour, depth/extra

struct VOut {
  float4 pos : SV_Position;
  float4 color : TEXCOORD0;
};

VOut vs_main(uint vid : SV_VertexID, uint iid : SV_InstanceID) {
  uint base = iid * 3;
  float4 rect = g_draws[base + 0];
  float4 color = g_draws[base + 1];
  float4 extra = g_draws[base + 2];
  float2 corners[6] = {
      float2(rect.x, rect.y),  // top-left
      float2(rect.x, rect.w),  // bottom-left
      float2(rect.z, rect.y),  // top-right
      float2(rect.z, rect.y),  // top-right
      float2(rect.x, rect.w),  // bottom-left
      float2(rect.z, rect.w),  // bottom-right
  };
  VOut o;
  o.pos = float4(corners[vid], extra.x, 1);
  o.color = color;
  return o;
}

float4 ps_main(VOut i) : SV_Target0 { return i.color; }
