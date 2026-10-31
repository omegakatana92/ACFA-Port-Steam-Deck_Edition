ByteAddressBuffer vb : register(t0);
Texture2D<float4> tex : register(t1);
SamplerState smp : register(s0);
cbuffer Params : register(b0) { float4 tint; };
cbuffer Params2 : register(b1) { float4 offset; };

struct VOut {
  float4 pos : SV_Position;
  float2 uv : TEXCOORD0;
};

VOut vs_main(uint id : SV_VertexID) {
  float4 q = asfloat(vb.Load4(0));  // (x0, y0, x1, y1)
  float2 corners[4] = {float2(q.x, q.y), float2(q.x, q.w), float2(q.z, q.y), float2(q.z, q.w)};
  float2 uv[4] = {float2(0, 0), float2(0, 1), float2(1, 0), float2(1, 1)};
  VOut o;
  float2 c = corners[id];
  o.pos = float4(c + offset.xy, 0, 1);
  o.uv = uv[id];
  return o;
}

float4 ps_main(VOut i) : SV_Target0 { return tex.Sample(smp, i.uv) * tint; }
