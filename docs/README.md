# Linux port documents

| Document | Contents | Status |
| --- | --- | --- |
| `LINUX_PORT_PLAN.md` | Phase 1 audit, Windows dependency inventory, AC4 comparison, phase gates | current |
| `BUILD_LOG.md` | Every build/link error, cause, fix and verification | current |
| `BUILDING_LINUX.md` | How to build and run the current development tree | current |
| `VULKAN_RENDERER.md` | Vulkan backend design: bindings, shaders, resources, presentation, gates | design; Stages 1–3 verified |
| `WRITE_TRACKING.md` | Guest-memory dirty tracking options and benchmark plan | design; not implemented |
| `OVERLAY.md` | PC SETTINGS/system-dialog 2D layer, font and input contract | design; stub only |
| `STEAM_DECK_AND_APPIMAGE.md` | Deck test matrix, AppImage contract, release checklist | plan; not started |

The audit and build log record only executed work. Design documents state explicitly what is not
implemented; do not read them as claims that a feature works.
