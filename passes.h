typedef struct PostFxSettings
{
	uint32_t dof_enabled;
	uint32_t fog_enabled;
	float    exposure;
	float    bloom_intensity;

	float    dof_focus_point;
	float    dof_focus_scale;
	float    dof_far_plane;

	float    dof_max_blur_size;
	float    dof_rad_scale;

	float    fog_density;
	float    fog_radius;
	float    fog_noise;
	float    fog_color[3];
	float    fog_center[3];
} PostFxSettings;

extern PostFxSettings g_postfx_settings;

void post_pass();
void pass_toon_outline();
void pass_analytic_fog();
void pass_smaa();
void pass_ldr_to_swapchain();
void pass_imgui();


