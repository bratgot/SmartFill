# nuke-ai-fill / menu.py
#
# Registers the AISmartFill node in Nuke's menus and toolbar.
#
# Installed location: <plugin_root>/menu.py, where the user's
# .nuke/menu.py adds the plugin root via nuke.pluginAddPath().

import nuke


def _register_smartfill():
    """Add AISmartFill to the Filter menu and the toolbar."""
    # Filter menu entry.
    filter_menu = nuke.menu("Nodes").findItem("Filter")
    if filter_menu is None:
        # Defensive: create the Filter menu if Nuke has not yet
        # populated it (rare, but happens with stripped configs).
        filter_menu = nuke.menu("Nodes").addMenu("Filter")
    filter_menu.addCommand(
        "AISmartFill",
        "nuke.createNode('AISmartFill')",
        icon="AISmartFill.png",
    )


def _register_aigenerate():
    """Stub - AIGenerate is not yet built; this hook lands later."""
    # Intentionally a no-op for now. The future AIGenerate Op will
    # register itself here.
    pass


_register_smartfill()
_register_aigenerate()
