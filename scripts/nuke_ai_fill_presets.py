# nuke-ai-fill / nuke_ai_fill_presets.py
#
# Presets for AIGenerate that auto-fill all required knobs for common
# workflows (txt2img, img2img, ControlNet, LoRA). Each preset gives the
# node a title (via label + on-panel header), shows user notes on a new
# 'Preset Info' tab, and exposes a 'Show help' button that pops up a
# usage dialog. Tile color is set by category for visual identification.
#
# USE
#   - Menu:  Image > AIGenerate Presets > {FLUX | SDXL} > <preset>
#            (creates a new node configured for that workflow)
#   - Apply: Image > AIGenerate Presets > Apply to selected > <preset>
#            (retargets the currently selected AIGenerate node)
#   - Code:  from nuke_ai_fill_presets import apply_preset
#            apply_preset(nuke.selectedNode(), 'flux_schnell_canny')
#
# AUDIT
#   To verify KNOB_NAMES against an actual node, select it and run:
#       from nuke_ai_fill_presets import dump_knobs
#       dump_knobs(nuke.selectedNode())
#   Or use the menu: AIGenerate Presets > Audit knobs on selected.

import os

import nuke


# --- Configuration ---------------------------------------------------------

MODELS_ROOT = os.path.expandvars(
    r"%USERPROFILE%/.nuke/nuke-ai-fill/models"
).replace("\\", "/")

# Logical-knob -> actual AIGenerate knob name. Verified against
# AIGenerate.dll knob dump (May 2026). Anything missing is silently
# skipped (with a Script Editor note) so partial mismatches don't
# break the rest.
KNOB_NAMES = {
    "architecture":         "model_type",        # Enumeration
    "diffusion_model":      "diffusion_model",
    "vae":                  "vae",
    "clip_l":               "clip_l",
    "t5xxl":                "t5xxl",
    "controlnet":           "control_net",
    "controlnet_strength":  "control_strength",
    "loras":                "loras",             # multiline text
    "loras_dir":            "loras_dir",
    "models_dir":           "models_dir",
    "prompt":               "prompt",
    "negative_prompt":      "negative_prompt",
    "width":                "width",
    "height":               "height",
    "steps":                "steps",
    "cfg":                  "cfg_scale",
    "seed":                 "seed",
    # 'strength' is the img2img denoise amount. img2img mode is auto-
    # detected by AIGenerate from input connectivity -- no separate
    # toggle knob exists.
    "strength":             "strength",
}

# Notes on knobs NOT in KNOB_NAMES and why:
#   - sampler / scheduler: sd.cpp auto-defaults these per the loaded
#     model (NDK_NOTES section 18). Don't override.
#   - img2img / denoise:   no such knobs. AIGenerate enters img2img
#     mode automatically when its image input is connected; the
#     'strength' Array_Knob controls denoise in that case.
#   - last_seed / cached / progress / status: read-only / status
#     knobs, not user-settable.


# --- Preset definitions ----------------------------------------------------
#
# Each preset:
#   title:    short name shown in menu, node label, dialog header
#   category: 'flux' | 'sdxl' | 'sd15'  (drives tile color + submenu)
#   notes:    multiline string shown on the Preset Info tab and in
#             the popup help dialog (\n preserved)
#   knobs:    {logical_key: value} pairs to push onto the node.
#             String values support {MODELS} as a placeholder for
#             MODELS_ROOT.

PRESETS = {

    "flux_schnell_txt2img": {
        "title": "FLUX schnell - txt2img (fast)",
        "category": "flux",
        "notes": (
            "Fast 4-step text-to-image with FLUX schnell.\n"
            "\n"
            "HOW TO USE\n"
            "  1. Leave the input UNCONNECTED (this is txt2img).\n"
            "  2. Type your prompt in the 'prompt' knob.\n"
            "  3. Press Bake.\n"
            "  4. ~5s per generation at 1024x1024 on RTX 5060 Ti.\n"
            "\n"
            "TIPS\n"
            "  - FLUX ignores negative prompts; leave blank.\n"
            "  - Try 8 steps for finer detail (still fast).\n"
            "  - Add (word:1.3) to emphasize a term."
        ),
        "knobs": {
            "architecture":     "FLUX schnell",
            "diffusion_model":  "{MODELS}/flux1-schnell-q4_0.gguf",
            "t5xxl":            "{MODELS}/t5-v1_1-xxl-encoder-Q8_0.gguf",
            "clip_l":           "{MODELS}/clip_l.safetensors",
            "vae":              "{MODELS}/ae.safetensors",
            "models_dir":       "{MODELS}",
            "controlnet":       "",
            "loras":            "",
            "loras_dir":        "{MODELS}/loras",
            "width":            1024,
            "height":           1024,
            "steps":            4,
            "cfg":              1.0,
            "seed":             -1,
        },
    },

    "flux_schnell_img2img": {
        "title": "FLUX schnell - img2img",
        "category": "flux",
        "notes": (
            "FLUX schnell img2img -- restyle or refine an existing image.\n"
            "\n"
            "HOW TO USE\n"
            "  1. Connect your source image to the AIGenerate input.\n"
            "     (Connection is what triggers img2img mode -- there's\n"
            "     no separate toggle.)\n"
            "  2. Type a prompt describing the desired result.\n"
            "  3. Adjust 'strength' (0.3-0.7 typical).\n"
            "  4. Press Bake.\n"
            "\n"
            "STRENGTH GUIDE\n"
            "  - 0.2-0.3: subtle refinement, composition preserved.\n"
            "  - 0.4-0.5: moderate restyle, still recognizable.\n"
            "  - 0.6-0.7: heavy restyle, structure intact.\n"
            "  - 0.8+   : near full regeneration."
        ),
        "knobs": {
            "architecture":     "FLUX schnell",
            "diffusion_model":  "{MODELS}/flux1-schnell-q4_0.gguf",
            "t5xxl":            "{MODELS}/t5-v1_1-xxl-encoder-Q8_0.gguf",
            "clip_l":           "{MODELS}/clip_l.safetensors",
            "vae":              "{MODELS}/ae.safetensors",
            "models_dir":       "{MODELS}",
            "controlnet":       "",
            "loras":            "",
            "loras_dir":        "{MODELS}/loras",
            "width":            1024,
            "height":           1024,
            "steps":            4,
            "cfg":              1.0,
            "strength":         0.5,
            "seed":             -1,
        },
    },

    "flux_schnell_realism": {
        "title": "FLUX schnell - photorealistic (LoRA)",
        "category": "flux",
        "notes": (
            "FLUX schnell with the XLabs realism LoRA applied at 1.0.\n"
            "\n"
            "HOW TO USE\n"
            "  1. Type a prompt - prefix with 'photo of...' for best\n"
            "     results.\n"
            "  2. Press Bake.\n"
            "\n"
            "TIPS\n"
            "  - Edit the 'loras' knob to change weight (format is\n"
            "    'name:weight', one entry per line, names relative to\n"
            "    loras_dir).\n"
            "  - Lower to 0.6-0.8 if results look too stylized.\n"
            "  - Combine with detailed scene descriptions.\n"
            "  - Still 4 steps; speed unaffected by LoRA.\n"
            "\n"
            "If the LoRA isn't applying, the syntax may be different in\n"
            "your build -- try '<lora:flux-realism:1.0>' or the full\n"
            "absolute path."
        ),
        "knobs": {
            "architecture":     "FLUX schnell",
            "diffusion_model":  "{MODELS}/flux1-schnell-q4_0.gguf",
            "t5xxl":            "{MODELS}/t5-v1_1-xxl-encoder-Q8_0.gguf",
            "clip_l":           "{MODELS}/clip_l.safetensors",
            "vae":              "{MODELS}/ae.safetensors",
            "models_dir":       "{MODELS}",
            "controlnet":       "",
            "loras":            "flux-realism:1.0",
            "loras_dir":        "{MODELS}/loras",
            "width":            1024,
            "height":           1024,
            "steps":            4,
            "cfg":              1.0,
            "seed":             -1,
        },
    },

    "flux_schnell_canny": {
        "title": "FLUX schnell - canny ControlNet",
        "category": "flux",
        "notes": (
            "FLUX schnell guided by canny edges.\n"
            "\n"
            "HOW TO USE\n"
            "  1. Connect the control image to the ControlNet input\n"
            "     (any image works; the plugin runs canny internally).\n"
            "  2. Type your prompt describing what the edges should\n"
            "     become.\n"
            "  3. Press Bake.\n"
            "\n"
            "STRENGTH GUIDE\n"
            "  - 0.4-0.5: loose, creative.\n"
            "  - 0.7    : balanced (default here).\n"
            "  - 0.9-1.0: strict adherence to edges."
        ),
        "knobs": {
            "architecture":         "FLUX schnell",
            "diffusion_model":      "{MODELS}/flux1-schnell-q4_0.gguf",
            "t5xxl":                "{MODELS}/t5-v1_1-xxl-encoder-Q8_0.gguf",
            "clip_l":               "{MODELS}/clip_l.safetensors",
            "vae":                  "{MODELS}/ae.safetensors",
            "models_dir":           "{MODELS}",
            "controlnet":           "{MODELS}/controlnets/diffusion_pytorch_model.safetensors",
            "controlnet_strength":  0.7,
            "loras":                "",
            "loras_dir":            "{MODELS}/loras",
            "width":                1024,
            "height":               1024,
            "steps":                4,
            "cfg":                  1.0,
            "seed":                 -1,
        },
    },

    "sdxl_txt2img": {
        "title": "SDXL - txt2img",
        "category": "sdxl",
        "notes": (
            "Standard SDXL text-to-image, 1024x1024, 20 steps.\n"
            "\n"
            "HOW TO USE\n"
            "  1. Leave the input UNCONNECTED (this is txt2img).\n"
            "  2. Type your prompt and (optional) negative prompt.\n"
            "  3. Press Bake. ~15s on RTX 5060 Ti.\n"
            "\n"
            "TIPS\n"
            "  - 30-50 steps for fine detail.\n"
            "  - CFG 5-9 typical; lower = more creative.\n"
            "  - 1024x1024 is the training resolution; off-square\n"
            "    aspect ratios work but quality dips below ~768."
        ),
        "knobs": {
            "architecture":     "SDXL",
            "diffusion_model":  "{MODELS}/sd_xl_base_1.0.safetensors",
            "models_dir":       "{MODELS}",
            "vae":              "",
            "clip_l":           "",
            "t5xxl":            "",
            "controlnet":       "",
            "loras":            "",
            "loras_dir":        "{MODELS}/loras",
            "width":            1024,
            "height":           1024,
            "steps":            20,
            "cfg":              7.0,
            "seed":             -1,
        },
    },

    "sdxl_img2img": {
        "title": "SDXL - img2img refine",
        "category": "sdxl",
        "notes": (
            "SDXL img2img -- refine or restyle an existing image.\n"
            "\n"
            "HOW TO USE\n"
            "  1. Connect your source image to the AIGenerate input.\n"
            "     (Connection is what triggers img2img mode -- there's\n"
            "     no separate toggle.)\n"
            "  2. Type a prompt describing the desired result.\n"
            "  3. Adjust 'strength' (0.3-0.7 typical).\n"
            "  4. Press Bake.\n"
            "\n"
            "STRENGTH GUIDE\n"
            "  - 0.2-0.3: subtle refinement, composition preserved.\n"
            "  - 0.4-0.5: moderate restyle, still recognizable.\n"
            "  - 0.6-0.7: heavy restyle, structure intact.\n"
            "  - 0.8+   : near full regeneration."
        ),
        "knobs": {
            "architecture":     "SDXL",
            "diffusion_model":  "{MODELS}/sd_xl_base_1.0.safetensors",
            "models_dir":       "{MODELS}",
            "vae":              "",
            "controlnet":       "",
            "loras":            "",
            "loras_dir":        "{MODELS}/loras",
            "width":            1024,
            "height":           1024,
            "steps":            20,
            "cfg":              7.0,
            "strength":         0.5,
            "seed":             -1,
        },
    },

    "sdxl_canny": {
        "title": "SDXL - canny ControlNet",
        "category": "sdxl",
        "notes": (
            "SDXL guided by canny edges. Uses the legacy-format\n"
            "ControlNet converted by tools/convert_sdxl_controlnet.py\n"
            "(see NDK_NOTES section 22).\n"
            "\n"
            "HOW TO USE\n"
            "  1. Connect your edge source (or any image) to the\n"
            "     ControlNet input.\n"
            "  2. Type your prompt.\n"
            "  3. Press Bake.\n"
            "\n"
            "TIPS\n"
            "  - Strength 0.7 is balanced.\n"
            "  - Higher CFG (8-10) helps prompt adherence under\n"
            "    strict edge guidance."
        ),
        "knobs": {
            "architecture":         "SDXL",
            "diffusion_model":      "{MODELS}/sd_xl_base_1.0.safetensors",
            "models_dir":           "{MODELS}",
            "vae":                  "",
            "controlnet":           "{MODELS}/controlnets/sdxl_canny_legacy.safetensors",
            "controlnet_strength":  0.7,
            "loras":                "",
            "loras_dir":            "{MODELS}/loras",
            "width":                1024,
            "height":               1024,
            "steps":                20,
            "cfg":                  8.0,
            "seed":                 -1,
        },
    },
}


# 0xRRGGBBAA. Roughly: FLUX orange, SDXL blue, SD1.5 green.
_CATEGORY_COLORS = {
    "flux":  0xFF8C00FF,
    "sdxl":  0x4080FFFF,
    "sd15":  0x80FF80FF,
}

_CATEGORY_LABELS = {
    "flux": "FLUX schnell",
    "sdxl": "SDXL",
    "sd15": "SD 1.5",
}

_CATEGORY_ORDER = ["flux", "sdxl", "sd15"]

# Logical knob keys that name files on disk (used by the pre-flight check)
_FILE_KEYS = ("diffusion_model", "vae", "clip_l", "t5xxl", "controlnet")


# --- Internals -------------------------------------------------------------

def _resolve(value):
    """Expand {MODELS} placeholder in string values."""
    if isinstance(value, str):
        return value.replace("{MODELS}", MODELS_ROOT)
    return value


def _set_knob(node, knob_name, value):
    """Set a knob safely. Returns True if it existed and accepted the
    value; False otherwise. Type-aware for bool/int/float/str."""
    k = node.knob(knob_name)
    if k is None:
        return False
    try:
        if isinstance(value, bool):
            k.setValue(1 if value else 0)
        elif isinstance(value, (int, float)):
            k.setValue(value)
        else:
            k.setValue(str(value))
        return True
    except Exception as e:
        nuke.tprint("[preset] failed to set {0}: {1}".format(knob_name, e))
        return False


def _safe_selected_node():
    try:
        return nuke.selectedNode()
    except ValueError:
        return None


def _missing_files(preset):
    """List file paths referenced by the preset's knobs that don't
    exist on disk. Empty strings are skipped (they mean 'unset')."""
    out = []
    for logical, value in preset["knobs"].items():
        if logical not in _FILE_KEYS:
            continue
        if not value:
            continue
        resolved = _resolve(value)
        if not os.path.exists(resolved):
            out.append(resolved)
    return out


def _ensure_preset_tab(node, preset_key):
    """Add or refresh a 'Preset Info' tab on the node showing title +
    notes and a 'Show help' button. Idempotent."""
    preset = PRESETS[preset_key]

    if node.knob("preset_info") is None:
        node.addKnob(nuke.Tab_Knob("preset_info", "Preset Info"))

    # Title (HTML-rendered Text_Knob)
    if node.knob("preset_title") is None:
        node.addKnob(nuke.Text_Knob("preset_title", ""))
    node.knob("preset_title").setValue(
        "<b><font size=4>" + preset["title"] + "</font></b>")

    # Notes (multiline). Re-create on each apply so the displayed text
    # always reflects the currently applied preset.
    if node.knob("preset_notes") is not None:
        node.removeKnob(node.knob("preset_notes"))
    nk = nuke.Multiline_Eval_String_Knob("preset_notes", "Notes")
    nk.setValue(preset["notes"])
    nk.setFlag(nuke.STARTLINE)
    node.addKnob(nk)

    # Help button
    if node.knob("preset_help") is None:
        cmd = ("from nuke_ai_fill_presets import show_preset_help; "
               "show_preset_help(nuke.thisNode())")
        node.addKnob(nuke.PyScript_Knob("preset_help", "Show help", cmd))

    # Hidden record of which preset is applied
    if node.knob("preset_key") is None:
        kk = nuke.String_Knob("preset_key", "")
        kk.setFlag(nuke.INVISIBLE)
        node.addKnob(kk)
    node.knob("preset_key").setValue(preset_key)


# --- Public API ------------------------------------------------------------

def apply_preset(node, preset_key, show_dialog=True):
    """Apply a preset's knob values to `node`. If `show_dialog`, pop
    the help dialog after applying."""
    if node is None:
        nuke.message("Select an AIGenerate node first.")
        return
    if node.Class() != "AIGenerate":
        nuke.message(
            "Selected node is not an AIGenerate node "
            "(it's a {0}).".format(node.Class()))
        return
    preset = PRESETS.get(preset_key)
    if preset is None:
        nuke.message("Unknown preset key: " + str(preset_key))
        return

    skipped = []
    for logical, value in preset["knobs"].items():
        knob_name = KNOB_NAMES.get(logical, logical)
        if not _set_knob(node, knob_name, _resolve(value)):
            skipped.append(knob_name)

    # Node label + tile color
    try:
        node["label"].setValue(preset["title"])
    except Exception:
        pass
    color = _CATEGORY_COLORS.get(preset["category"])
    if color is not None:
        try:
            node["tile_color"].setValue(color)
        except Exception:
            pass

    _ensure_preset_tab(node, preset_key)

    if skipped:
        nuke.tprint(
            "[preset {0}] these knobs were not found and were "
            "skipped: {1}".format(preset_key, ", ".join(skipped)))

    missing = _missing_files(preset)
    if missing:
        msg = ("Preset '{0}' applied, but these model files were not "
               "found:\n\n  - {1}\n\nDownload them and place under:\n  "
               "{2}\n\nThe Bake will fail with a load error until the\n"
               "files are present.").format(
            preset["title"], "\n  - ".join(missing), MODELS_ROOT)
        nuke.message(msg)
        return

    # Force a panel repaint. Nuke's panel widget caches its rendered
    # state and doesn't always re-render when knobs are set
    # programmatically after the panel was opened. Toggling the
    # panel visibility forces a fresh paint with the new values.
    try:
        if node.shown():
            node.hideControlPanel()
            node.showControlPanel()
    except Exception:
        pass

    if show_dialog:
        show_preset_help(node)


def create_with_preset(preset_key):
    """Create a new AIGenerate node, apply the preset, leave it selected
    with its panel open and populated."""
    # inpanel=False prevents the panel from auto-opening before we've
    # filled in the knob values; otherwise the panel can render once
    # with empty values and not repaint after setValue.
    n = nuke.createNode("AIGenerate", inpanel=False)
    apply_preset(n, preset_key, show_dialog=True)
    # Ensure the panel renders with the populated values.
    try:
        n.hideControlPanel()
    except Exception:
        pass
    n.showControlPanel()


def show_preset_help(node):
    """Display the popup help dialog for whichever preset is applied
    to `node`."""
    if node is None:
        nuke.message("No node.")
        return
    pk = node.knob("preset_key")
    key = pk.value() if pk is not None else ""
    preset = PRESETS.get(key)
    if preset is None:
        nuke.message(
            "No preset is currently applied to this node.\n\n"
            "Apply one from:\n"
            "  Image > AIGenerate Presets > Apply to selected")
        return
    nuke.message("{0}\n\n{1}".format(preset["title"], preset["notes"]))


def dump_knobs(node):
    """Print every knob name + type on `node`. Use to verify
    KNOB_NAMES mappings against an actual AIGenerate node."""
    if node is None:
        nuke.tprint("[preset] no node")
        return
    print("--- knobs on {0} ({1}) ---".format(node.name(), node.Class()))
    for name in sorted(node.knobs().keys()):
        k = node.knob(name)
        print("  {0:32s}  {1}".format(name, type(k).__name__))
    print("--- end ---")


def dump_enum(node, knob_name):
    """Print the enumeration values for an Enumeration_Knob. Useful
    when guessing what 'model_type' accepts."""
    if node is None:
        nuke.tprint("[preset] no node")
        return
    k = node.knob(knob_name)
    if k is None:
        nuke.tprint("[preset] no knob: " + knob_name)
        return
    try:
        print("--- enum values for {0}.{1} ---".format(
            node.name(), knob_name))
        for v in k.values():
            print("  '{0}'".format(v))
        print("--- end ---")
    except AttributeError:
        nuke.tprint("[preset] {0} is not an Enumeration_Knob "
                    "({1})".format(knob_name, type(k).__name__))


# --- Menu registration -----------------------------------------------------

def register_preset_menu():
    """Wire presets into Image > AIGenerate Presets. Idempotent enough
    for menu.py to call on every Nuke startup."""
    image_menu = nuke.menu("Nodes").findItem("Image")
    if image_menu is None:
        image_menu = nuke.menu("Nodes").addMenu("Image")

    presets_menu = image_menu.findItem("AIGenerate Presets")
    if presets_menu is None:
        presets_menu = image_menu.addMenu("AIGenerate Presets")

    grouped = {}
    for key, preset in PRESETS.items():
        grouped.setdefault(preset["category"], []).append((key, preset))

    # Create-new submenus, one per category
    for cat in _CATEGORY_ORDER:
        if cat not in grouped:
            continue
        sub = presets_menu.findItem(_CATEGORY_LABELS[cat])
        if sub is None:
            sub = presets_menu.addMenu(_CATEGORY_LABELS[cat])
        for key, preset in grouped[cat]:
            cmd = ("from nuke_ai_fill_presets import create_with_preset; "
                   "create_with_preset({0!r})".format(key))
            sub.addCommand(preset["title"], cmd)

    presets_menu.addSeparator()

    # Apply-to-selected submenu
    apply_sub = presets_menu.findItem("Apply to selected")
    if apply_sub is None:
        apply_sub = presets_menu.addMenu("Apply to selected")
    for cat in _CATEGORY_ORDER:
        if cat not in grouped:
            continue
        for key, preset in grouped[cat]:
            cmd = ("from nuke_ai_fill_presets import "
                   "_menu_apply_to_selected; "
                   "_menu_apply_to_selected({0!r})".format(key))
            apply_sub.addCommand(preset["title"], cmd)

    presets_menu.addSeparator()
    presets_menu.addCommand(
        "Show help for selected",
        "from nuke_ai_fill_presets import show_preset_help; "
        "show_preset_help(nuke.selectedNode())")
    presets_menu.addCommand(
        "Audit knobs on selected",
        "from nuke_ai_fill_presets import dump_knobs; "
        "dump_knobs(nuke.selectedNode())")


def _menu_apply_to_selected(preset_key):
    """Menu thunk: apply a named preset to whatever AIGenerate node is
    currently selected, or warn if none."""
    node = _safe_selected_node()
    if node is None:
        nuke.message("Select an AIGenerate node first.")
        return
    apply_preset(node, preset_key, show_dialog=True)
