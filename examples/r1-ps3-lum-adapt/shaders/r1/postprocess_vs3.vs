// Pass-through vertex shader for R1 post-process passes that use ps_3_0.
// D3D9 only runs a ps_3_0 together with a vs_3_0. When a post-process .s names a vertex shader (instead of "null"),
// the engine (r1-ps3-lum-adapt build) sends the full-screen quad already in clip space (POSITION, w = 1), with the
// same two colours and three texture coordinates the old XYZRHW quad carried. So this only copies them through.
// main_vs_2_0 is the fallback the engine picks on a GPU without shader model 3 (then the .ps must offer main_ps_2_0).

struct vi
{
	float4 P : POSITION;
	float4 c0 : COLOR0;
	float4 c1 : COLOR1;
	float2 t0 : TEXCOORD0;
	float2 t1 : TEXCOORD1;
	float2 t2 : TEXCOORD2;
};

struct vo
{
	float4 P : POSITION;
	float4 c0 : COLOR0;
	float4 c1 : COLOR1;
	float2 t0 : TEXCOORD0;
	float2 t1 : TEXCOORD1;
	float2 t2 : TEXCOORD2;
};

vo pass_through(vi I)
{
	vo O;
	O.P = I.P;
	O.c0 = I.c0;
	O.c1 = I.c1;
	O.t0 = I.t0;
	O.t1 = I.t1;
	O.t2 = I.t2;
	return O;
}

vo main_vs_3_0(vi I) { return pass_through(I); }
vo main_vs_2_0(vi I) { return pass_through(I); }
