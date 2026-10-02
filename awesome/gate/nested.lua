-- nested.lua — a throwaway Hyprland config for the nested dev instance
-- (Hyprland-in-a-window). Loads the repo's plugin directly. NO autostart,
-- NO portal/dbus restarts, NO exec-once — nothing that could touch the
-- live session. Runs under its own dbus session and Wayland backend.
--
-- The glass·ink theme ships as the plugin's C++ defaults (core/theme.hpp),
-- so the nested instance sets NO plugin values — it tests exactly the
-- defaults the design decided. Blur is on because the islands/cards are
-- glass.

-- load the plugin we BUILD in the repo, so the nested instance tests local
-- changes — not the stale hyprpm cache. Override the source dir with
-- HYPR_PLUGIN_DIR.
local REPO = os.getenv("HYPR_PLUGIN_DIR") or (os.getenv("HOME") .. "/repo/hyprland-plugins")

-- the nested output: a window in the live session. Reserve the band.
hl.monitor({
	output = "",
	mode = "1280x800@60",
	position = "0x0",
	scale = 1,
	reserved = { top = 30 },
})

hl.config({
	general = { gaps_in = 0, gaps_out = 0, border_size = 1 },
	-- blur 20 ≈ size 5 x 2 passes; the glass needs it
	decoration = { shadow = { enabled = false }, blur = { enabled = true, size = 5, passes = 2 } },
	animations = { enabled = false },
	misc = {
		disable_hyprland_logo = true,
		disable_splash_rendering = true,
		-- the vanilla awesome baseline (permissions.activate): an activation
		-- ask on a visible window focuses it. The batteries test this mode;
		-- never inject/restore the value at runtime (2026-10-02 stuck line).
		focus_on_activate = true,
		font_family = "IBM Plex Sans",
		-- Keep it near-black (<=5): the stress panel-bottom detector keys on
		-- min-channel > 5 to find the panel's edge, so the background must sit
		-- below that threshold. The glass still samples a non-zero surface.
		background_color = 0x050505, -- dark enough that panel_bottom resolves the edge
	},
})

-- $mod = ALT here, never SUPER: the live session owns the SUPER chords, and
-- the nested instance only sees input while its window is focused. A few
-- binds for poking at it by hand.
hl.bind("ALT + Return", hl.dsp.exec_cmd("foot"), { description = "terminal" })
hl.bind("ALT + Q", hl.dsp.exit(), { description = "quit nested" })
hl.bind("ALT + SHIFT + C", hl.dsp.window.close(), { description = "close window" })
hl.bind("ALT + P", function()
	if hl.plugin and hl.plugin.awesome then hl.plugin.awesome.menubar() end
end, { description = "menubar" })
hl.bind("ALT + N", function()
	if hl.plugin and hl.plugin.awesome then hl.plugin.awesome.center() end
end, { description = "notification center" })

-- the monolith: one plugin; the input pipeline owns the order
hl.plugin.load(REPO .. "/awesome/awesome.so")
