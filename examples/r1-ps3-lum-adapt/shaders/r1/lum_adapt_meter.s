-- OPTIONAL. Without this file the engine binds s_image = $user$rendertarget itself.
-- Ship it when the meter needs more samplers, e.g. the NVG tube mask (then weight the taps in lum_adapt_meter.ps).
-- If you name a vertex shader here instead of "null" (for a ps_3_0 meter), it gets the clip-space quad too.

function normal(shader, t_base, t_second, t_detail)
	shader:begin("null", "lum_adapt_meter")
		:fog(false)
		:zb(false, false)
	shader:sampler("s_image") :texture("$user$rendertarget") :clamp() :f_linear()
	-- shader:sampler("s_mask") :texture("shaders\\my_tube_mask") :clamp() :f_linear()
end
