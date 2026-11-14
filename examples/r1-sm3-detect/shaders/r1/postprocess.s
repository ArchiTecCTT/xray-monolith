-- R1 post-process that uses ps_3_0 where the engine supports it and the vanilla path everywhere else.
-- Copy the same branch into postprocess_d.s (and postprocess_cm.s / postprocess_dcm.s if you override those).
--
-- r1_sm3_postprocess is set by the engine before any .s runs:
--   true  = this exe compiles main_ps_3_0 and sends the clip-space quad to a pass that names a vertex shader
--   false = this exe, but the GPU has no vs 3.0 / ps 3.0
--   nil   = an older exe (it never sets it; an undefined global simply reads nil here)

function normal(shader, t_base, t_second, t_detail)
	if r1_sm3_postprocess then
		shader:begin("postprocess_vs3", "postprocess")   -- pass-through vs_3_0 + main_ps_3_0
	else
		shader:begin("null", "postprocess")              -- vanilla: XYZRHW quad + main (ps_2_0)
	end
	shader:fog(false)
	shader:zb(false, false)

	shader:sampler("s_base0")     :texture("$user$rendertarget") :clamp() :f_linear()
	shader:sampler("s_base1")     :texture("$user$rendertarget") :clamp() :f_linear()
	shader:sampler("s_noise")     :texture("fx\\fx_noise2")      :f_linear()
	-- Samplers only the ps_3_0 entry reads. Binding a sampler the compiled shader does not have is a no-op, so this
	-- is harmless on the ps_2_0 path too; the `if` only makes the intent obvious.
	if r1_sm3_postprocess then
		shader:sampler("s_lum_adapt") :texture("$user$lum_adapt") :clamp() :f_none()
	end
end
