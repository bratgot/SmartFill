# nuke-ai-fill / menu.py
#
# Registers AISmartFill and AIGenerate, drives status text updates
# via forceValidate polling while bakes are active.
#
# KNOWN LIMITATION: After a bake completes ("Ready" status), the Nuke
# viewer does NOT automatically refresh to show the result. This is a
# long-documented Nuke quirk (Foundry mailing list threads going back
# to 2012) where async Op completions don't propagate to the viewer
# widget. Manual workaround:
#   - Click any knob field and press Enter, OR
#   - Disconnect/reconnect the viewer's input, OR
#   - Run in Script Editor:
#       nuke.toNode('AIGenerate1').knob('bump_cook_revision').execute()
# Any of these will load the freshly-baked image into the viewer.

import threading

import nuke

_MANAGED_OP_CLASSES = ("AISmartFill", "AIGenerate")


def _register_nodes():
    image_menu = nuke.menu("Nodes").findItem("Image")
    if image_menu is None:
        image_menu = nuke.menu("Nodes").addMenu("Image")

    filter_menu = nuke.menu("Nodes").findItem("Filter")
    if filter_menu is None:
        filter_menu = nuke.menu("Nodes").addMenu("Filter")

    filter_menu.addCommand("AISmartFill",
                           "nuke.createNode('AISmartFill')")
    image_menu.addCommand("AIGenerate",
                          "nuke.createNode('AIGenerate')")

_register_nodes()

_POLL_INTERVAL_S = 0.3
_active_status_markers = ("Cooking", "Writing", "Loading",
                          "Starting", "Preparing", "Inferencing",
                          "Generating")


def _is_active(val):
    return any(marker in val for marker in _active_status_markers)


def _poll_step():
    """Main thread. Walk managed nodes; forceValidate any whose status
    reports active work. This drives status text updates and ensures
    poll_worker() runs in C++ to detect worker thread completion."""
    try:
        for n in nuke.allNodes():
            try:
                if n.Class() not in _MANAGED_OP_CLASSES:
                    continue
                sk = n.knob("status")
                if sk is None:
                    continue
                val = sk.value()
                if _is_active(val):
                    n.forceValidate()
            except Exception:
                pass
    except Exception:
        pass

    timer = threading.Timer(
        _POLL_INTERVAL_S,
        lambda: nuke.executeInMainThread(_poll_step))
    timer.daemon = True
    timer.start()


nuke.executeInMainThread(_poll_step)
