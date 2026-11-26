-- DisplayXR for Hyprland (Omarchy) -- written by displayxr-hyprland-setup.
-- Hyprland 0.55+ Lua configuration. Edit freely, but the setup replaces this
-- file only while the "managed" line below is still here.
-- displayxr-hyprland-setup: managed
--
-- Windows that show a weave must reach the panel pixel for pixel: a border,
-- rounded corners, a shadow, blur, dimming, transparency or an open/close
-- animation resamples or covers the interlaced image and breaks the 3D.
-- DisplayXR = the runtime's own XWayland window (WM_CLASS class),
-- displayxr-service = its native Wayland window (app_id).

hl.window_rule({
	name = "displayxr-weave",
	match = { class = "^(DisplayXR|displayxr-service)$" },
	border_size = 0,
	rounding = 0,
	no_shadow = true,
	no_blur = true,
	no_dim = true,
	opaque = true,
	no_anim = true,
})

-- Workspace-controller launch hotkey: bind it to
--   @PREFIX@/bin/displayxr-cli workspace launch <id>
-- (ids: displayxr-cli workspace list).
