/**************************************************************************/
/*  ddgi.h                                                                */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#pragma once

#include "core/math/aabb.h"
#include "core/math/vector3i.h"
#include "core/string/string_name.h"
#include "core/templates/rid.h"
#include "servers/rendering/renderer_rd/shaders/environment/ddgi_resolve.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/raytracing/ddgi_gbuffer_raygen.glsl.gen.h"
#include "servers/rendering/renderer_rd/storage_rd/render_buffer_custom_data_rd.h"

#define RB_SCOPE_DDGI SNAME("ddgi")

class RenderDataRD;
class RenderSceneBuffersRD;

namespace RendererSceneRenderImplementation {
class RenderRaytracing;
struct RTSceneSnapshot;
} // namespace RendererSceneRenderImplementation

//和RTXGI对齐
#define RB_TEX_GBUFFER_A SNAME("albedo_flags")
#define RB_TEX_GBUFFER_B SNAME("position_hit_t")
#define RB_TEX_GBUFFER_C SNAME("normal")
#define RB_TEX_GBUFFER_D SNAME("direct_diffuse")
#define RB_TEX_GBUFFER_DEBUG SNAME("debug")

namespace RendererRD {

struct DDGISettings {
	Vector3i probe_count = Vector3i(16, 8, 16);
	Vector3 probe_spacing = Vector3(2.0, 2.0, 2.0);
	uint32_t rays_per_probe = 64;
	float max_ray_distance = 20.0f;
	float hysteresis = 0.97f;
	float normal_bias = 0.2f;
	float view_bias = 0.1f;
	float energy = 1.0f;
	bool read_sky = true;

	bool is_valid() const;
	bool has_same_structure(const DDGISettings &p_other) const;
};

struct DDGIFrameGrid {
	Vector3 origin;
	Vector3i probe_count;
	Vector3 probe_spacing;
	// Signed cell movement from previous_grid.origin to this origin. A normal
	// integer scroll preserves history for the future probe core to remap.
	Vector3i scroll_delta;
	AABB probe_bounds;
	AABB expanded_bounds;
	bool valid = false;
};

/*
configure(p_render_buffers)：将 DDGIState 绑定到指定视口的 RenderSceneBuffersRD。
重复绑定同一对象时不处理；切换对象前会先 free_data()。
它本身不分配 GPU 资源，资源由 ensure_probe_resources() 延迟创建。
free_data()：释放 DDGI 的所有 GPU RID（探针 buffer、双缓冲 irradiance/distance atlas），
清空历史、网格、环境等状态，并解除与 render_buffers 的绑定。
*/
class DDGIState : public RenderBufferCustomDataRD {
	GDCLASS(DDGIState, RenderBufferCustomDataRD);

	RenderSceneBuffersRD *render_buffers = nullptr;
	bool resource_failure_reported = false;
	void _free_resources();

public:
	DDGISettings settings;
	RID environment;
	RID scenario;
	DDGIFrameGrid current_grid;
	DDGIFrameGrid previous_grid;
	uint64_t prepared_scene_pass = 0;
	uint64_t committed_scene_pass = 0;
	uint64_t snapshot_generation = 0;
	uint32_t current_atlas_index = 0;
	bool history_reset = true;
	bool resources_ready = false;

	// Persistent per-viewport probe resources. The facade owns shader pipelines,
	// while the render-buffer state owns every resource carrying probe history.
	RID grid_uniform_buffer;
	RID ray_data;
	RID probe_state_buffer;
	RID irradiance_atlas[2];
	RID distance_atlas[2];

	void prepare_frame(const DDGISettings &p_settings, RID p_environment, RID p_scenario, const DDGIFrameGrid &p_grid, uint64_t p_scene_pass);
	bool ensure_probe_resources();
	// Called only after the diagnostic resolve was successfully recorded.
	void commit_frame(uint64_t p_snapshot_generation);
	bool is_configured_for(const RenderSceneBuffersRD *p_render_buffers) const;
	bool is_prepared_for_scene_pass(uint64_t p_scene_pass) const;
	bool is_committed_for_scene_pass(uint64_t p_scene_pass) const;

	virtual void configure(RenderSceneBuffersRD *p_render_buffers) override;
	virtual void free_data() override;
	~DDGIState();
};

class DDGI {
	struct GBufferPushConstant {
		uint32_t light_count;
		uint32_t frame_index;
		float normal_bias;
		float view_bias;
		uint32_t debug_mode;
		float pad[3];
		float debug_bounds_min[4];
		float debug_bounds_inv_size[4];
	};

	struct ResolvePushConstant {
		int32_t screen_size[2];
		float energy;
		float pad;
	};

	DdgiGbufferRaygenShaderRD gbuffer_shader;
	RID gbuffer_shader_version;
	RID gbuffer_pipeline;
	RID gbuffer_hit_sbt;
	bool use_radiance_octmap_array = false;

	DdgiResolveShaderRD resolve_shader;

	RID resolve_shader_version;
	RID resolve_pipeline;

	static DDGIFrameGrid _build_frame_grid(const DDGISettings &p_settings, const Vector3 &p_camera_position);

public:
	DDGI(bool p_use_radiance_octmap_array);
	~DDGI();

	bool prepare_frame(const Ref<RenderSceneBuffersRD> &p_render_buffers, const DDGISettings &p_settings, RID p_environment, RID p_scenario, const Vector3 &p_camera_position, uint64_t p_scene_pass, AABB &r_expanded_bounds);
	bool clear_state(const Ref<RenderSceneBuffersRD> &p_render_buffers);

	bool ensure_ddgi_gbuffer_textures(const Ref<RenderSceneBuffersRD> &p_render_buffers, bool p_clear_existing = false);
	bool clear_ddgi_gbuffer_textures(const Ref<RenderSceneBuffersRD> &p_render_buffers);

	// Manages DDGIState::ray_data: one 16-byte result per probe/ray pair.
	bool ensure_ddgi_probe_irradiance_buffer(const Ref<RenderSceneBuffersRD> &p_render_buffers, bool p_clear_existing = false);
	bool clear_ddgi_probe_irradiance_buffer(const Ref<RenderSceneBuffersRD> &p_render_buffers);

	bool ensure_gi_outputs(const Ref<RenderSceneBuffersRD> &p_render_buffers, bool p_clear_existing = false);
	bool clear_gi_outputs(const Ref<RenderSceneBuffersRD> &p_render_buffers);

	bool update_ddgi_gbuffer(
			const Ref<DDGIState> &p_state,
			const RendererSceneRenderImplementation::RTSceneSnapshot &p_snapshot,
			RendererSceneRenderImplementation::RenderRaytracing &p_rt_service,
			const RenderDataRD *p_render_data,
			RID p_sky_radiance);

	// Black-box boundary for the future probe core. Start its compute list here,
	// bind DDGI-owned descriptors, and call
	// RenderRaytracing::register_compute_dependencies() before dispatching any
	// shader that dereferences scene buffers through device addresses.
	bool update_probes(
			const Ref<DDGIState> &p_state,
			const RendererSceneRenderImplementation::RTSceneSnapshot &p_snapshot,
			RendererSceneRenderImplementation::RenderRaytracing &p_rt_service,
			const RenderDataRD *p_render_data,
			RID p_sky_radiance);

	// Diagnostic screen resolve. This deliberately does not implement DDGI
	// sampling; it only proves the raster GI-buffer connection.
	bool resolve(
			const Ref<DDGIState> &p_state,
			const Ref<RenderSceneBuffersRD> &p_render_buffers,
			RID p_depth,
			RID p_normal_roughness);
};

} // namespace RendererRD
