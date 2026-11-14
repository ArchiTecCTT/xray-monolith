-- R1 post-process, ps_3_0 version. Copy the same change into postprocess_d.s (distortion variant), and into
-- postprocess_cm.s / postprocess_dcm.s if your mod overrides those too.
-- Naming a vertex shader here (not "null") is what makes the engine send the clip-space quad.

function normal(shader, t_base, t_second, t_detail)
	shader:begin("postprocess_vs3", "postprocess")
		:fog(false)
		:zb(false, false)
	shader:sampler("s_base0")     :texture("$user$rendertarget") :clamp() :f_linear()
	shader:sampler("s_base1")     :texture("$user$rendertarget") :clamp() :f_linear()
	shader:sampler("s_noise")     :texture("fx\\fx_noise2")      :f_linear()
	shader:sampler("s_lum_adapt") :texture("$user$lum_adapt")    :clamp() :f_none()
end
