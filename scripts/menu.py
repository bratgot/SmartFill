# nuke-ai-fill / menu.py
#
# Registers AISmartFill and AIGenerate, drives status text updates
# via forceValidate polling while bakes are active.
#
# The polling bumps cook_revision before each forceValidate(). This
# is mandatory: without the bump, forceValidate() from a polling
# timer is a no-op when the C++ side hasn't dirtied a real knob
# since the last validate (Nuke's internal cache-dirty check ignores
# append()-only changes). With the bump, status text updates live
# during the bake AND the Viewer auto-refreshes to the baked image
# when the worker completes -- no manual nudge needed.
#
# See NDK_NOTES section 24 for the full diagnostic and section 14
# for the original viewer-refresh problem this resolves.

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
    """Main thread. Walk managed nodes; for any whose status reports
    active work, bump cook_revision and forceValidate. The bump is
    mandatory -- see NDK_NOTES section 24. It dirties Nuke's
    validate cache AND propagates as a hash change (cook_revision
    is included in the C++ append() override) so the Viewer
    re-cooks once the worker flips state_ to Ready."""
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
                    # Bump cook_revision before forceValidate. Without
                    # this, forceValidate() is a no-op when the C++
                    # side hasn't dirtied a real knob since the last
                    # validate (the cache-dirty check ignores
                    # append()-only changes). See NDK_NOTES section 24.
                    cr = n.knob("cook_revision")
                    if cr is not None:
                        cr.setValue(cr.value() + 1)
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
