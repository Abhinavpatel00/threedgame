typedef struct PostFxSettings
{
	uint32_t dof_enabled;
	float    exposure;
	float    bloom_intensity;

	float    dof_focus_point;
	float    dof_focus_scale;
	float    dof_far_plane;

	float    dof_max_blur_size;
	float    dof_rad_scale;
} PostFxSettings;

extern PostFxSettings g_postfx_settings;

void post_pass();
void pass_toon_outline();
void pass_smaa();
void pass_ldr_to_swapchain();
void pass_imgui();


