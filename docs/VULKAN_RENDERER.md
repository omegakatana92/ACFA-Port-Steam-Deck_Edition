# Vulkan renderer — design and staging

Status: **not implemented**. This document specifies how the D3D11 RSX renderer will be replaced
by a native Vulkan backend. It is a design document, not a report of working rendering. The only
Vulkan code that exists today is `src/runtime/gpu/vulkan/vk_clear_test.cpp`, which verifies
instance/device/image/clear/readback on lavapipe (see `docs/BUILD_LOG.md` entry 10).

Targets: Vulkan 1.3 on SteamOS (RDNA 2), with a Vulkan 1.1 compatibility path where practical.

---

## 1. Scope and non-goals

**In scope**

- RSX command stream → Vulkan draw calls.
- Surfaces, render targets, depth/stencil, MRT and feedback copies.
- Textures (linear, swizzled/Morton, DXT, float), samplers and LOD behaviour.
- Generated HLSL → SPIR-V with the exact descriptor layout the generated code expects.
- Vertex streams decoded in the vertex shader from raw guest bytes.
- Clears (including partial channel masks), fullscreen passes, render-scale re-creation.
- Occlusion queries and report values.
- Presentation: swapchain, aspect-fit blit, captures, overlay composition.
- The `gpu::display` timing contract (vblank, frame pacing stats).

**Out of scope for the first renderer milestone**

- Ray tracing, variable-rate shading, mesh shaders.
- HDR output and colour management (game output is LDR).
- Multi-GPU / device groups.
- An ARM/NEON port.

---

## 2. Backend boundary

The current renderer mixes three concerns:

1. RSX state tracking and resource lifetime (`renderer.cpp`).
2. A D3D11-shaped command recorder (`submit.cpp`).
3. Device/swapchain/presentation (`d3d11_device.cpp`).

The Vulkan backend must keep (1) and replace (2)+(3). A backend interface should expose only what
the renderer actually uses:

```
class GraphicsBackend {
  // resources
  Texture create_texture(TextureDesc);
  Buffer create_buffer(BufferDesc);
  Sampler create_sampler(SamplerDesc);
  Pipeline create_graphics_pipeline(PipelineDesc);
  QueryPool create_occlusion_pool(uint32_t count);
  // frame
  void begin_frame();
  void begin_render(AttachmentSet);
  void set_viewport(Viewport);
  void set_scissor(Rect);
  void bind_pipeline(Pipeline);
  void bind_vertex_streams(std::span<BufferBinding>);
  void bind_index_buffer(Buffer, uint32_t offset);
  void bind_uniforms(Stage, uint32_t slot, BufferBinding);
  void bind_textures(Stage, uint32_t first, std::span<TextureView>);
  void bind_samplers(Stage, uint32_t first, std::span<Sampler>);
  void draw(uint32_t count, uint32_t first);
  void draw_indexed(uint32_t count, uint32_t first, int32_t base_vertex);
  void clear_attachment(Attachment, ClearValue, Rect);
  void copy_image(Image dst, Image src);
  void update_buffer(Buffer, uint64_t offset, const void* data, size_t bytes);
  void begin_query(QueryPool, uint32_t index);
  void end_query(QueryPool, uint32_t index);
  bool query_result(QueryPool, uint32_t index, uint64_t* value);  // non-blocking
  void end_render();
  void end_frame();
  // presentation
  void present(Image src, Rect src_rect, Rect dst_rect);
};
```

Opaque handles must be cheap to copy and stable. The renderer's `release()`/recorded-call lifetime
rules become explicit frame-scoped resource retirement: a resource is destroyed after the fence of
the last frame that used it.

**D3D11 escape hatches to remove:** `Recorder::call()`/`call_sync()` (six sites) rely on an
immediate context and `Get*` state queries. Replace them with explicit operations:

| Site | Replacement |
| --- | --- |
| `fullscreen_pass` | `begin_render` with a pipeline and a fullscreen triangle |
| guest-memory present upload | staging buffer → image copy |
| depth→colour | fullscreen pass sampling depth/stencil views |
| zpass begin/end | `begin_query`/`end_query` |
| present | backend `present()` |
| zpass poll | `query_result` with a non-blocking poll |

---

## 3. Shader path: HLSL → SPIR-V

### 3.1 Compiler

Use DirectX Shader Compiler (DXC) with the SPIR-V backend:

```
dxc -T vs_6_2 -E main -spirv -fspv-target-env=vulkan1.3 \
    -fvk-use-dx-layout \
    -fvk-b-shift 0 0 -fvk-t-shift 1000 0 -fvk-s-shift 2000 0 -fvk-u-shift 3000 0 \
    -Fo <hash>.spv <generated.hlsl>
```

Why SM 6.2: `vp_hlsl.cpp` emits `ByteAddressBuffer.Load4`, introduced in SM 6.2. SM 6.0/6.1
compilation fails.

Why the shifts: the generated HLSL reuses binding numbers across descriptor types and stages:

- VS: `t0..t3` = `ByteAddressBuffer` (vertex streams), `b0..b2` = uniforms.
- PS: `t0..t15` = `Texture2D/3D/Cube`, `b2..b3` = uniforms, `s0..s15` = samplers.
- The same `t0` is a storage buffer in the VS and a sampled image in the PS.

Vulkan requires unique `(set, binding)` pairs. The shifts put buffers at 0+, textures at 1000+,
samplers at 2000+, UAVs at 3000+, all in set 0. The pipeline layout is generated from the same
table, and a reflection test must assert:

- no binding is used with two descriptor types;
- no binding is used with incompatible stages;
- all statically used bindings exist in the layout.

`-fvk-use-dx-layout` keeps D3D cbuffer packing for the `float4`/`uint4` arrays.

### 3.2 Cache

Cache key: source hash, stage, entry point, DXC version, target environment and shift values.
Store SPIR-V next to the existing DXBC cache but in a separate directory
(`shader_cache/spv/<sha256>.spv`) so Windows and Linux never collide. `shader_warm.cpp` can run
the same loop without a device; add a `--warm-shaders` implementation once DXC is linked.

### 3.3 Y-flip

`vp_hlsl.cpp` writes the D3D convention: NDC y up, framebuffer y down, with an explicit flip in the
vertex shader (`res.pos = float4((win.x*rt.w*2-1)*w, (1-win.y*rt.h*2)*w, win.z*w, w)`).

Options:

1. **Negative viewport height** (`viewport.height = -h`, requires Vulkan 1.1/maintenance1) and
   reverse `frontFace` because winding is evaluated in framebuffer space. No shader changes.
2. **Generator epilogue** selected by the backend, removing the flip before DXC.

Recommendation: option 1 for the first renderer (it keeps one shader source and the Windows path
unchanged), with a synthetic orientation test that proves triangle winding, culling and texture
orientation. If any shader relies on `SV_Position` signs in ways that break with a negative
viewport, switch to option 2.

### 3.4 Other shader risks

- **Out-of-range raw loads:** `vb_load` reads `[q, q+20)`; D3D returns 0 beyond a bound range,
  Vulkan does not. Pad every vertex-stream storage range by at least 32 bytes and/or clamp `q` in
  the shader.
- **`f16tof32`/`f32tof16`:** verify DXC SPIR-V output on the target driver; they are bit-level
  conversions and should map to `OpBitcast`/`OpExtInst`.
- **Dynamic indexing / loops / `switch(pc)`:** the vertex interpreter uses a dynamic switch and a
  small stack array. Validate with a synthetic branching VP.
- **`discard` in a called function**, `SV_Depth`, MRT outputs, cube samplers, comparison samplers:
  validate with synthetic programs before game data.
- **Descriptor indexing of 16 samplers:** use a fixed layout with 16 sampler bindings; no
  `descriptorIndexing` requirement.

---

## 4. Resource mapping

### 4.1 Surfaces

| D3D11 | Vulkan |
| --- | --- |
| `ID3D11Texture2D` (RT/DS) | `VkImage` (`COLOR_ATTACHMENT`/`DEPTH_STENCIL_ATTACHMENT`, `SAMPLED`, `TRANSFER_SRC/DST`) |
| `ID3D11RenderTargetView` | `VkImageView` (colour aspect) |
| `ID3D11DepthStencilView` | `VkImageView` (depth/stencil aspects) |
| `ID3D11ShaderResourceView` | `VkImageView` + descriptor |
| feedback `copy` texture | second `VkImage` + `vkCmdCopyImage` |
| depth-as-colour | colour image produced by a fullscreen pass sampling depth/stencil |

- Storage size remains game size × render scale; cap at 8192.
- Format map: `R16G16B16A16_SFLOAT`, `R32G32B32A32_SFLOAT`, `R32_SFLOAT`, otherwise
  `R8G8B8A8_UNORM`; depth `D16_UNORM` or `D24_UNORM_S8_UINT`.
- Query `VkFormatProperties`; if `R32G32B32A32_SFLOAT` cannot be a colour attachment, fall back to
  `R16G16B16A16_SFLOAT` and log once.
- Layout transitions: `UNDEFINED → COLOR_ATTACHMENT` on first use, `COLOR_ATTACHMENT →
  SHADER_READ_ONLY` after a pass, `SHADER_READ_ONLY → TRANSFER_SRC` for feedback, etc. Use
  dynamic rendering (Vulkan 1.3) with explicit barriers; no render-pass objects needed.
- Clears: full clear via `vkCmdClearColorImage` before the pass, or `loadOp=CLEAR`; **partial
  channel masks and scissored clears must remain a fullscreen triangle with a write mask**, because
  Vulkan clears cannot honour a channel mask.

### 4.2 Textures

- Keep `rsx_texture.cpp`'s CPU decode/conversion. Replace `DXGI_FORMAT` in `TexData` with a neutral
  `TexFormat` mapped to `VkFormat`.
- Upload through a staging buffer + `vkCmdCopyBufferToImage`, then transition to
  `SHADER_READ_ONLY_OPTIMAL`.
- Immutable textures are created once; changed textures release the old image after the frame
  fence and create a new one (matching current D3D behaviour).
- Render-to-texture aliasing: if a texture address equals a surface, bind the surface's view.
- Samplers map 1:1: filter, mipmap mode, address modes, anisotropy, LOD bias, min/max LOD, border
  colour, and the inverted comparison function for shadow samplers.
- Cube/3D textures use `VK_IMAGE_VIEW_TYPE_CUBE`/`3D`; verify face order and Morton layout against
  the CPU converter.

### 4.3 Buffers

- Vertex streams: 64 MiB ring minimum, `STORAGE_BUFFER` usage, per-frame slots. No
  `WRITE_NO_OVERWRITE`: record the slot and wait on the frame fence before reuse.
- Index buffer: `R32_UINT`, `INDEX_BUFFER` usage.
- Constant buffers: host-visible uniform buffers in per-frame rings, 256-byte alignment.
  `vc[512]` = 8 KiB, `fc[256]` = 4 KiB, both below the 64 KiB `maxUniformBufferRange`.
  `FpState` and `VtxFmt` are small.
- `UpdateConstants` becomes a memcpy into the mapped ring; no partial-update emulation.
- Static/cached streams reuse immutable buffers as today.

### 4.4 Pipeline state

Bake immutable state into `VkPipeline`, keep viewport/scissor/stencil reference dynamic.

| D3D11 | Vulkan |
| --- | --- |
| blend enable / factors / ops / write mask / independent blend | `VkPipelineColorBlendAttachmentState[]` + `independentBlendEnable` |
| depth test / write / func | `VkPipelineDepthStencilStateCreateInfo` |
| stencil ops / masks / two-sided | front/back stencil state |
| fill mode wireframe | `fillModeNonSolid` (feature query) |
| cull mode / front face | `rasterizerDiscardEnable=false`, `cullMode`, `frontFace` (invert if negative viewport) |
| `DepthClipEnable=false` | `depthClampEnable=true` (requires `depthClamp`) |
| depth bias | `depthBiasConstantFactor`/`SlopeFactor`, scaled by render scale |
| scissor | dynamic `VK_DYNAMIC_STATE_SCISSOR` |
| viewport | dynamic `VK_DYNAMIC_STATE_VIEWPORT` (negative height) |
| primitive topology | `VkPrimitiveTopology`; restart via index values / split lists as today |

Pipeline cache keyed by a hash of the descriptor structs plus shader handles; mirror
`StateCache`'s interning.

---

## 5. Frame and presentation

### 5.1 Frame model

- 2–3 swapchain images, one command buffer/fence per frame in flight.
- Acquire → record → submit → present.
- Resize/fullscreen: recreate swapchain after `vkDeviceWaitIdle`; skip zero-sized extents.
- `VK_KHR_present_wait` (if available) can improve pacing; otherwise use the monotonic clock from
  `display_clock.cpp` and keep the game's 60 Hz rule outside missions/movies.

### 5.2 Swapchain

- Prefer `B8G8R8A8_UNORM` or `R8G8B8A8_UNORM` with `SRGB_NONLINEAR` colour space.
- Present modes: `FIFO` default; `MAILBOX` when the user chooses uncapped; `IMMEDIATE` for the
  clock/tearing pacing mode if the surface supports it.
- `VK_EXT_surface_maintenance1` can express tearing explicitly; do not assume it exists.
- Aspect-fit blit: compute the destination rectangle exactly as `present_texture` does today
  (letterbox/pillarbox, integer scale not required). Clear the unused area black.
- Captures: copy the swapchain image (or the last composed image) to a host-visible buffer after
  present; keep the existing non-blocking `begin_capture`/`poll_captures` contract.

### 5.3 Overlay

The overlay is drawn after the game blit, into the same swapchain image, with its own pipeline and
descriptor state. See `docs/OVERLAY.md`.

---

## 6. Synchronisation and threading

- The RSX FIFO thread records; a submission thread submits, as today. Vulkan command pools must be
  externally synchronised per thread (one pool per recording thread).
- Resource destruction is deferred until the frame fence. `rec().release()` becomes
  `retire(resource, frame_index)`.
- Guest memory written by the CPU must be visible before a draw reads it: use a host-write →
  transfer-read barrier and, for cached guest data, the existing `renderer_guest_sync` contract.
- No Vulkan calls from the PPU/SPU threads.

---

## 7. Verification gates (no game data)

| Gate | Test |
| --- | --- |
| G0 | Headless instance/device/clear/readback (**passes today**) |
| G1 | SDL swapchain, resize, borderless/fullscreen toggle, present a CPU-generated pattern (**passes today on lavapipe + Xvfb**) |
| G2 | DXC compiles a synthetic VS/PS with `Load4`, texture sample, branch loop; reflection matches the pipeline layout |
| G3 | Synthetic triangle/quad: orientation, culling, texture coordinates, negative viewport |
| G4 | State matrix: blend/depth/stencil/cull/scissor vs CPU reference |
| G5 | Surface/clear/feedback tests; render scale 50/100/200 % |
| G6 | Texture decode tests: linear, swizzled, DXT1/3/5, float, cube, mip |
| G7 | Full draw path: vertex streams, MRT, depth-as-colour, occlusion counts |
| G8 | Overlay walkthrough + screenshots |
| G9 | Real game: title, movie, mission, garage, menus, save/load |
| G10 | Steam Deck: 1280×800, 60 fps, controller-only, suspend/resume |

G9/G10 require the user's own game data and Steam Deck hardware. No gate past G1 has been run.

### 7.1 G1 evidence (2026-10-31)

`src/runtime/gpu/vulkan/vk_presenter.cpp` creates the SDL Vulkan surface, device and swapchain,
uploads a CPU test pattern, letterboxes it on black, presents, and recreates the swapchain on
resize. `acfa-vk-present` drives 180 frames with a 960×600 resize, a return to 1280×800, and a
borderless fullscreen toggle:

```
presenter ready: 1280x800
frame verification: OK
presented 180 frames
```

Run under Xvfb with Mesa lavapipe (`SDL_VIDEODRIVER=x11`,
`VK_ICD_FILENAMES=.../lvp_icd.x86_64.json`). `--test-present` exercises the same path from the
runtime. This is **not** Steam Deck/RADV testing, does not compile shaders, and does not draw game
geometry.

---

## 8. Risks

| Risk | Impact | Mitigation |
| --- | --- | --- |
| Binding-space collisions | nothing renders | shifts + reflection assertions |
| Y-flip/winding | mirrored or invisible geometry | negative viewport + orientation test |
| Raw reads out of range | garbage/UB | pad ranges; clamp |
| DXC `Load4` support | compile failure | pin DXC; fallback generator for SM 6.0 |
| Format support | surface creation failure | query and fall back |
| No write tracking | texture hashing cost | see `docs/WRITE_TRACKING.md` |
| Ring overwrite | corruption | per-frame slots + fences |
| Occlusion query semantics | wrong conditional draws | CPU-decided draws; validate counts |
| Driver differences (AMD vs llvmpipe) | visual differences | test on Deck; log format/feature gaps |

---

## 9. Work breakdown

1. Backend interface + neutral format enum; keep D3D11 compiling.
2. SDL surface/swapchain + present of a CPU pattern.
3. DXC integration + shader cache + reflection test.
4. Pipeline/resource layer; synthetic draw.
5. Surface/clear/feedback; render scale.
6. Textures/samplers.
7. Full RSX draw path and queries.
8. Overlay.
9. Write tracking and performance.
10. Steam Deck validation.

No AppImage work starts before gate G9.
