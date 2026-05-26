#!/usr/bin/env python3
"""
convert_sdxl_controlnet.py

Convert a diffusers-format SDXL ControlNet .safetensors file to the legacy
ComfyUI/A1111 naming that stable-diffusion.cpp's ControlNet loader expects.

Usage:
    python convert_sdxl_controlnet.py input.safetensors output.safetensors

Why this exists:
    stable-diffusion.cpp has a name-conversion table for the UNet base that
    maps diffusers names to legacy. The ControlNet loader does NOT have this
    table. Most SDXL ControlNets on HuggingFace ship with diffusers naming
    (down_blocks.*, controlnet_cond_embedding.*, add_embedding.*,
    time_embedding.*) which sd.cpp rejects with hundreds of "unknown tensor"
    warnings followed by "not in model file" errors. This script renames the
    tensors so the file loads.

Dependencies:
    pip install safetensors numpy

Tested against:
    - diffusers/controlnet-canny-sdxl-1.0/diffusion_pytorch_model.safetensors
    - lllyasviel/sd_control_collection/diffusers_xl_canny_full.safetensors
    - lllyasviel/sd_control_collection/sai_xl_canny_full (where applicable)

Conversion summary:
    controlnet_cond_embedding.conv_in     -> input_hint_block.0
    controlnet_cond_embedding.blocks.0-5  -> input_hint_block.{2,4,6,8,10,12}
    controlnet_cond_embedding.conv_out    -> input_hint_block.14
    controlnet_down_blocks.0-8            -> zero_convs.{0-8}.0
    controlnet_mid_block                  -> middle_block_out.0
    conv_in                               -> input_blocks.0.0
    time_embedding.linear_1/2             -> time_embed.0/2
    add_embedding.linear_1/2              -> label_emb.0.0/0.2
    down_blocks.X.resnets.Y.*             -> input_blocks.<idx>.0.*  (renamed)
    down_blocks.X.attentions.Y.*          -> input_blocks.<idx>.1.*
    down_blocks.X.downsamplers.0.conv.*   -> input_blocks.<idx>.0.op.*
    mid_block.resnets.0/1.*               -> middle_block.0/2.*  (renamed)
    mid_block.attentions.0.*              -> middle_block.1.*

A separate fix: proj_in/proj_out weights are reshaped from 2D [out,in]
to 4D [out,in,1,1]. Diffusers SDXL stores them as nn.Linear (because
use_linear_projection=True); legacy ComfyUI/sd.cpp expects them as
1x1 nn.Conv2d. Mathematically equivalent, storage layout differs.

SDXL down-stage layout for the index math:
    down_blocks.0: 2 resnets + downsampler                (320  ch)
    down_blocks.1: 2 resnets + 2 attentions + downsampler (640  ch)
    down_blocks.2: 2 resnets + 2 attentions               (1280 ch)
    ->  9 legacy input_blocks (0-8). input_blocks.0 is conv_in.
"""

import argparse
import sys
from pathlib import Path

from safetensors import safe_open
from safetensors.numpy import save_file


# ---- resnet sub-key conversion ---------------------------------------------

_RESNET_SUB = {
    "conv1.weight":         "in_layers.2.weight",
    "conv1.bias":           "in_layers.2.bias",
    "norm1.weight":         "in_layers.0.weight",
    "norm1.bias":           "in_layers.0.bias",
    "conv2.weight":         "out_layers.3.weight",
    "conv2.bias":           "out_layers.3.bias",
    "norm2.weight":         "out_layers.0.weight",
    "norm2.bias":           "out_layers.0.bias",
    "time_emb_proj.weight": "emb_layers.1.weight",
    "time_emb_proj.bias":   "emb_layers.1.bias",
    "conv_shortcut.weight": "skip_connection.weight",
    "conv_shortcut.bias":   "skip_connection.bias",
}

# ---- index maps -------------------------------------------------------------

# (down_blocks_idx, resnet_idx) -> legacy input_blocks index
_RESNET_TO_IB = {
    (0, 0): 1, (0, 1): 2,
    (1, 0): 4, (1, 1): 5,
    (2, 0): 7, (2, 1): 8,
}

# (down_blocks_idx, attention_idx) -> legacy input_blocks index
_ATTN_TO_IB = {
    (1, 0): 4, (1, 1): 5,
    (2, 0): 7, (2, 1): 8,
}

# down_blocks_idx -> legacy input_blocks index for its downsampler
_DOWN_TO_IB = {0: 3, 1: 6}

# controlnet_cond_embedding.blocks.<i> -> input_hint_block.<j>
_COND_EMBED_TO_HINT = {0: 2, 1: 4, 2: 6, 3: 8, 4: 10, 5: 12}


# ---- key conversion --------------------------------------------------------

def convert_key(k):
    """Return the legacy name for a diffusers key, or None if unmapped."""

    # conv_in (top-level)
    if k.startswith("conv_in."):
        return "input_blocks.0.0." + k[len("conv_in."):]

    # time embedding
    if k.startswith("time_embedding.linear_1."):
        return "time_embed.0." + k[len("time_embedding.linear_1."):]
    if k.startswith("time_embedding.linear_2."):
        return "time_embed.2." + k[len("time_embedding.linear_2."):]

    # SDXL add_embedding -> label_emb
    if k.startswith("add_embedding.linear_1."):
        return "label_emb.0.0." + k[len("add_embedding.linear_1."):]
    if k.startswith("add_embedding.linear_2."):
        return "label_emb.0.2." + k[len("add_embedding.linear_2."):]

    # controlnet_cond_embedding -> input_hint_block
    if k.startswith("controlnet_cond_embedding.conv_in."):
        return "input_hint_block.0." + k[len("controlnet_cond_embedding.conv_in."):]
    if k.startswith("controlnet_cond_embedding.conv_out."):
        return "input_hint_block.14." + k[len("controlnet_cond_embedding.conv_out."):]
    if k.startswith("controlnet_cond_embedding.blocks."):
        rest = k[len("controlnet_cond_embedding.blocks."):]
        idx_s, _, suffix = rest.partition(".")
        idx = int(idx_s)
        if idx in _COND_EMBED_TO_HINT:
            return f"input_hint_block.{_COND_EMBED_TO_HINT[idx]}.{suffix}"
        return None

    # zero-convs on the down path
    if k.startswith("controlnet_down_blocks."):
        rest = k[len("controlnet_down_blocks."):]
        idx_s, _, suffix = rest.partition(".")
        return f"zero_convs.{idx_s}.0.{suffix}"

    # mid-block zero-conv
    if k.startswith("controlnet_mid_block."):
        return "middle_block_out.0." + k[len("controlnet_mid_block."):]

    # down_blocks.X.{resnets|attentions|downsamplers}.*
    if k.startswith("down_blocks."):
        rest = k[len("down_blocks."):]
        block_s, _, after = rest.partition(".")
        kind, _, after2 = after.partition(".")
        block_i = int(block_s)

        if kind == "resnets":
            rn_s, _, sub = after2.partition(".")
            ib = _RESNET_TO_IB.get((block_i, int(rn_s)))
            if ib is None:
                return None
            legacy_sub = _RESNET_SUB.get(sub)
            if legacy_sub is None:
                return None
            return f"input_blocks.{ib}.0.{legacy_sub}"

        if kind == "attentions":
            attn_s, _, sub = after2.partition(".")
            ib = _ATTN_TO_IB.get((block_i, int(attn_s)))
            if ib is None:
                return None
            return f"input_blocks.{ib}.1.{sub}"

        if kind == "downsamplers":
            ib = _DOWN_TO_IB.get(block_i)
            if ib is None:
                return None
            # "0.conv.weight" or "0.conv.bias"
            _, _, suffix = after2.partition("conv.")
            return f"input_blocks.{ib}.0.op.{suffix}"

        return None

    # mid_block.{resnets|attentions}.*
    if k.startswith("mid_block."):
        rest = k[len("mid_block."):]
        kind, _, after = rest.partition(".")

        if kind == "resnets":
            rn_s, _, sub = after.partition(".")
            rn_i = int(rn_s)
            mb = 0 if rn_i == 0 else 2
            legacy_sub = _RESNET_SUB.get(sub)
            if legacy_sub is None:
                return None
            return f"middle_block.{mb}.{legacy_sub}"

        if kind == "attentions":
            _, _, sub = after.partition(".")
            return f"middle_block.1.{sub}"

        return None

    return None


# ---- main ------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(
        description="Convert diffusers SDXL ControlNet safetensors to "
                    "legacy A1111/sd.cpp naming.",
    )
    ap.add_argument("input", help="Input .safetensors (diffusers format)")
    ap.add_argument("output", help="Output .safetensors (legacy format)")
    ap.add_argument("-v", "--verbose", action="store_true",
                    help="Print all key mappings (verbose)")
    ap.add_argument("--force", action="store_true",
                    help="Overwrite output file if it exists")
    args = ap.parse_args()

    in_path = Path(args.input)
    out_path = Path(args.output)

    if not in_path.exists():
        print(f"ERROR: input file not found: {in_path}", file=sys.stderr)
        return 2
    if out_path.exists() and not args.force:
        print(f"ERROR: output exists: {out_path} (use --force to overwrite)",
              file=sys.stderr)
        return 2

    print(f"Reading {in_path}")
    print(f"  ({in_path.stat().st_size / (1024*1024):.1f} MB)")

    tensors = {}
    unmapped = []
    reshaped = []
    metadata = {}

    with safe_open(str(in_path), framework="np", device="cpu") as f:
        meta = f.metadata()
        if meta:
            metadata.update(meta)
        keys = list(f.keys())
        print(f"  {len(keys)} input tensors")
        for k in keys:
            new_k = convert_key(k)
            if new_k is None:
                unmapped.append(k)
                continue
            t = f.get_tensor(k)
            # SDXL stores proj_in / proj_out as nn.Linear (2D [out, in])
            # because use_linear_projection=True. Legacy ComfyUI/sd.cpp
            # expects them as 1x1 nn.Conv2d (4D [out, in, 1, 1]).
            # Reshape only the .weight tensors; .bias is already 1D.
            if (new_k.endswith(".proj_in.weight") or
                new_k.endswith(".proj_out.weight")) and t.ndim == 2:
                t = t.reshape(t.shape[0], t.shape[1], 1, 1)
                reshaped.append(new_k)
            tensors[new_k] = t
            if args.verbose:
                print(f"  {k}  ->  {new_k}")

    print(f"\nConverted {len(tensors)} tensors.")
    if reshaped:
        print(f"Reshaped {len(reshaped)} proj_in/proj_out weights from "
              f"2D Linear -> 4D Conv 1x1.")
    if unmapped:
        print(f"\nWARNING: {len(unmapped)} tensors had no mapping and were "
              f"dropped:")
        for k in unmapped[:30]:
            print(f"  {k}")
        if len(unmapped) > 30:
            print(f"  ... and {len(unmapped) - 30} more")
        print("\nIf any of these look essential, the mapping table needs "
              "extending.")

    metadata["converted_from"] = "diffusers"
    metadata["converted_by"] = "convert_sdxl_controlnet.py"

    print(f"\nWriting {out_path}")
    save_file(tensors, str(out_path), metadata=metadata)
    out_mb = out_path.stat().st_size / (1024 * 1024)
    print(f"  ({out_mb:.1f} MB) done.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
