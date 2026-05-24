# nuke-ai-fill / menu.py
#
# Registers AISmartFill and AIGenerate and runs a continuous main-thread
# polling loop that calls forceValidate() on any node reporting active
# work. Why continuous (vs trigger-on-knob-change): C++ knob_changed
# mutates the status knob via set_text(), which does NOT reliably fire
# the Python addKnobChanged callback (Nuke has re-entry guards).
# Continuous polling is bulletproof and cheap.
#
# Additional responsibility: when a node transitions out of cooking,
# nudge any active viewer to recook so the user sees the freshly-baked
# result without manually disconnecting/reconnecting an input.

import threading

import nuke

# Op classes we manage. Add new Ops here as the project grows.
_MANAGED_OP_CLASSES = ("AISmartFill", "AIGenerate")

# ----------------------------------------------------------------------
# Node registration
# ----------------------------------------------------------------------

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

# ----------------------------------------------------------------------
# Continuous polling loop
# ----------------------------------------------------------------------

_POLL_INTERVAL_S = 0.3
_active_status_markers = ("Cooking", "Writing", "Loading",
                          "Starting", "Preparing", "Inferencing",
                          "Generating")

# Tracks per-node previous status to detect transitions.
_prev_status = {}


def _is_active(val):
    return any(marker in val for marker in _active_status_markers)


def _nudge_viewer():
    """Force the active viewer to recook. Used after a node transitions
    out of cooking state, to make the freshly-baked result visible
    without the user having to manually disconnect/reconnect inputs."""
    try:
        v = nuke.activeViewer()
        if v is None:
            return
        # Frame-nudge
        try:
            cur = nuke.frame()
            nuke.frame(cur)
        except Exception:
            pass
        # Touch hide_input on the viewer node to force redraw
        try:
            vnode = v.node()
            if vnode is not None:
                k = vnode.knob('hide_input')
                if k is not None:
                    k.setValue(k.value())
        except Exception:
            pass
    except Exception:
        pass


def _poll_step():
    """Main thread. Walk all managed nodes; forceValidate any whose
    status reports active work. On transition out of active, nudge
    the viewer to recook."""
    try:
        transitioned_to_ready = False
        for n in nuke.allNodes():
            try:
                if n.Class() not in _MANAGED_OP_CLASSES:
                    continue
                sk = n.knob("status")
                if sk is None:
                    continue
                val = sk.value()
                key = n.fullName()
                was_active = _prev_status.get(key, "")
                was_active_flag = _is_active(was_active)

                if _is_active(val):
                    n.forceValidate()
                    val = sk.value()

                if was_active_flag and not _is_active(val):
                    transitioned_to_ready = True

                _prev_status[key] = val
            except Exception:
                pass

        if transitioned_to_ready:
            _nudge_viewer()

    except Exception:
        pass

    timer = threading.Timer(
        _POLL_INTERVAL_S,
        lambda: nuke.executeInMainThread(_poll_step))
    timer.daemon = True
    timer.start()


# Kick off the loop at module load.
nuke.executeInMainThread(_poll_step)
