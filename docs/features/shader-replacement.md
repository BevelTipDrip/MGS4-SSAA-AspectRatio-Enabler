# Replaced shaders under DirectX 12

**Setting:** `Replaced Shaders (DirectX 12)`
**Implements:** `asi/src/features/shader_port.cpp`, the `Hooked_CreateGraphicsPipelineState`
path in `asi/src/features/render_pipeline.cpp`, `shaders/ReplacedShadersPS.dx12.map`

## What it does

Makes another mod's pixel-shader replacements work when the game runs on DirectX 12.
MGS4.FusionFix ships 519 shadow-related pixel shaders rewritten with 3Dmigoto (a spiral
shadow filter in place of the game's), as DXBC blobs in `MGS4\scripts\ReplacedShadersPS`,
each named by the 16-byte checksum in the original shader's DXBC header, and swaps them in
from a hook on the DirectX 11 device's `CreatePixelShader`. Under DirectX 12 the shader
arrives inside a pipeline-state description instead, so nothing of that runs.

## Why re-keying was not enough (measured 2026-09-07)

Lab probes on both devices (`Hooked_CreatePixelShader11`, the pipeline hook), one boot each
to the same stage:

| | DirectX 11 | DirectX 12 |
| --- | --- | --- |
| unique pixel shaders to gameplay | 751 | 717 |
| FusionFix targets among them | 509 | 0 |
| compiled programs in common | 24 | 24 |

Both shader packs (`common/shaders/vfp_PC.1.pak`, `.2.pak`) are DirectX 11 style DXBC, shader
model 5.0, LZ4-style compressed; FusionFix's checksums appear in both as literals inside the
compressed headers, which is what first suggested the sets were shared. They are not: the
DirectX 12 originals are stripped containers (ISGN, OSGN, SHEX only) with different
checksums and different code hashes.

Dumping both sets (`Dump Pixel Shaders` lab key, `logs\dx11_pixel_shaders` and
`logs\dx12_pixel_shaders`) and matching them by declarations and signatures, the difference
turned out to be small and regular. For a matched pair the code chunks are the same length,
and the dwords that differ are exactly:

- the size in `dcl_constantbuffer` (e.g. `cb0[31]` becomes `cb0[196]`, `cb0[26]` becomes
  `cb0[61]`), and
- every immediate constant index, shifted by that same difference (165 or 35).

So the DirectX 12 build lays the same constants out after a common block. The input
signatures also differ: the DirectX 12 vertex shaders output `TEXCOORD4..6` as `xyz`, where the
DirectX 11 ones output `xyzw`, and a pixel shader declaring `xyzw` there is refused by
`CreateGraphicsPipelineState` (debug layer: "Signatures between stages are incompatible ...
component mask that is not a subset of the output of the previous stage").

## The port

1. **Which shader is which.** A replacement keeps its original's declarations, so it is
   fingerprinted by its input and output signature chunks plus the declaration instructions
   at the head of the code (resources, samplers, inputs, outputs; constant-buffer sizes and
   temp counts left out). 82 distinct fingerprints cover the 519 replacements against 209 for
   the 717 DirectX 12 shaders, so the fingerprint alone is ambiguous; the DirectX 11 original's
   code size (equal to the DirectX 12 one's) breaks the tie. That needed the DirectX 11
   originals, which only a DirectX 11 boot provides, hence the map is generated offline and
   shipped: `shaders/ReplacedShadersPS.dx12.map`, 458 of 519 (61 have no unambiguous
   counterpart). A replacement's own size cannot break the tie: the rewritten filter makes
   it about 1.5 KB larger than its original.
2. **The port itself** (`shaderport::Port`, mirrored in the scratchpad's `binport.py`): walk
   the replacement's token stream, shift every immediate constant-buffer index by the
   per-slot size difference, set the declared sizes to the DirectX 12 original's, copy the
   DirectX 12 original's ISGN chunk in, recompute the container checksum (D3D12 verifies it:
   MD5 with Microsoft's own final block, reproduced on 40 real containers). The token walker
   handles extended opcode and operand tokens, relative and immediate-plus-relative indexing
   (the immediate base shifts), immediates, and custom-data blocks (opcode 0x35; a first cut
   used 0x33, which is `min`, and skipped most of every shader).
3. **At run time** the DirectX 12 device hook reads FusionFix's folder and the map, and the
   pipeline hook ports each replacement the first time a pipeline is built with its DirectX 12
   original, caches it, and swaps it in. A refused pipeline falls back to the original and is
   logged. A `ReplacedShadersPS12` folder of ready-made DirectX 12 blobs is used directly.

**Verified**, DirectX 12 through the override, same stage as the DirectX 11 comparison: over
600 pipelines swapped, none refused, and the gameplay capture shows the same softened shadow
edges as the DirectX 11 run with FusionFix (both with the fxc-recompiled set and with the
Python binary port installed as a `ReplacedShadersPS12` folder). Before the input-signature
fix every pipeline was refused; before the index shift they were accepted and the scene
rendered black with rim highlights only (constants read from the wrong slots). The ASI's own
`shaderport::Port` was then built standalone (`scratchpad/porttest`) and run over all 458
mapped pairs: its output is byte-identical to the Python port's, checksums included. The
map-driven path in the game itself then ran to gameplay: 458 ported, over 600 pipelines
swapped, none refused. Its first build crashed the user's launch at the first shadow
pipeline: the hook re-enters itself for the swapped description and held a std::mutex across
that call (locking twice on one thread throws; the game's crash log showed 0xe06d7363 raised
from the ASI). The lock now covers only the port and the lookup.

## Offline tooling (scratchpad, not shipped)

`match_shaders.py` (fingerprint + size match), `port_shaders.py` (recompile the HLSL sources
with fxc after shifting `cb0[...]`, the first working port), `binport.py` (the binary port,
what the ASI does), `sign_shaders.py` (checksum). They read the lab dumps and FusionFix's
installed files. The map is regenerated only if the game's shader build changes.

---

## Active bugs

None recorded.

## Not done

- The 61 unmatched replacements stay DirectX 11 only.
- FusionFix's ini overrides (dynamic resolution, shadow size) were not examined on DirectX 12.
