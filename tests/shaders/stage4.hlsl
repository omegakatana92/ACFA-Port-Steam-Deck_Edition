// Stage 4 surface shader: fullscreen textured quad with a per-draw tint and depth.
StructuredBuffer<float4> g_draws : register(t0);  // tint, uv bias, depth/extra
Texture2D<float4> g_tex : register(t1);
SamplerState g_smp : register(s0);

struct VOut {
  float4 pos : SV_Position;
  float2 uv : TEXCOORD0;
  nointerpolation uint iid : TEXCOORD1;
};

VOut vs_main(uint vid : SV_VertexID, uint iid : SV_InstanceID) {
  uint base = iid * 3;
  float4 extra = g_draws[base + 2];
  float2 p[6] = {
      float2(-1, 1), float2(-1, -1), float2(1, 1),
      float2(1, 1),  float2(-1, -1), float2(1, -1),
  };
  float2 uv[6] = {
      float2(0, 0), float2(0, 1), float2(1, 0),
      float2(1, 0), float2(0, 1), float2(1, 1),
  };
  VOut o;
  o.pos = float4(p[vid], extra.x, 1);
  o.uv = uv[vid];
  o.iid = iid;
  return o;
}

float4 ps_main(VOut i) : SV_Target0 {
  float4 tint = g_draws[i.iid * 3 + 0];
  return g_tex.Sample(g_smp, i.uv) * tint;
}
