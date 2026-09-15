/**************************************************************************/
/*  ddgi.cpp                                                              */
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

#include "ddgi.h"

#include "core/error/error_macros.h"
#include "core/math/math_funcs.h"
#include "servers/rendering/renderer_rd/environment/gi.h"
#include "servers/rendering/renderer_rd/forward_clustered/render_raytracing.h"
#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/mesh_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/render_data_rd.h"
#include "servers/rendering/renderer_rd/storage_rd/render_scene_buffers_rd.h"
#include "servers/rendering/renderer_rd/storage_rd/texture_storage.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"
#include "servers/rendering/rendering_device.h"
#include "servers/rendering/storage/environment_storage.h"

namespace RendererRD {

namespace {

constexpr uint32_t DDGI_IRRADIANCE_TEXELS = 8;
constexpr uint32_t DDGI_DISTANCE_TEXELS = 16;
constexpr uint32_t DDGI_ATLAS_BORDER = 1;
constexpr uint32_t DDGI_RAY_RESULT_STRIDE = 16;
constexpr uint32_t DDGI_PROBE_STATE_STRIDE = 32;
constexpr uint64_t DDGI_MAX_BUFFER_BYTES = 256 * 1024 * 1024;
constexpr uint64_t DDGI_MAX_ATLAS_BYTES = 256 * 1024 * 1024;
constexpr uint64_t DDGI_MAX_TOTAL_BYTES = 512 * 1024 * 1024;

struct DDGIResourceLayout {
	uint32_t irradiance_width = 0;
	uint32_t irradiance_height = 0;
	uint32_t distance_width = 0;
	uint32_t distance_height = 0;
	uint32_t layers = 0;
	uint32_t ray_result_bytes = 0;
	uint32_t probe_state_bytes = 0;
};

// std140-compatible. This is intentionally broader than the diagnostic
// resolve needs so the future probe core can consume the same grid UBO.
struct alignas(16) DDGIGridData {
	float origin[4] = {};
	int32_t probe_count[4] = {};
	float probe_spacing[4] = {};
	int32_t scroll_delta[4] = {};
	float max_ray_distance = 0.0f;
	float hysteresis = 0.0f;
	float normal_bias = 0.0f;
	float view_bias = 0.0f;
	float energy = 0.0f;
	uint32_t rays_per_probe = 0;
	uint32_t frame_index = 0;
	uint32_t current_atlas_index = 0;
	uint32_t history_reset = 0;
};

static_assert((sizeof(DDGIGridData) % 16) == 0, "DDGIGridData must remain std140 aligned.");

bool _checked_mul(uint64_t p_a, uint64_t p_b, uint64_t &r_result) {
	if (p_a != 0 && p_b > UINT64_MAX / p_a) {
		return false;
	}
	r_result = p_a * p_b;
	return true;
}

bool _checked_add(uint64_t p_a, uint64_t p_b, uint64_t &r_result) {
	if (p_b > UINT64_MAX - p_a) {
		return false;
	}
	r_result = p_a + p_b;
	return true;
}

bool _calculate_resource_layout(const DDGISettings &p_settings, DDGIResourceLayout &r_layout, String *r_error = nullptr) {
	auto fail = [&](const String &p_error) {
		if (r_error) {
			*r_error = p_error;
		}
		return false;
	};

	if (!p_settings.is_valid()) {
		return fail("invalid DDGI settings");
	}

	const uint64_t count_x = uint64_t(p_settings.probe_count.x);
	const uint64_t count_y = uint64_t(p_settings.probe_count.y);
	const uint64_t count_z = uint64_t(p_settings.probe_count.z);

	uint64_t probe_xy = 0;
	uint64_t probe_count = 0;
	if (!_checked_mul(count_x, count_y, probe_xy) ||
			!_checked_mul(probe_xy, count_z, probe_count)) {
		return fail("probe-count product overflow");
	}

	uint64_t ray_count = 0;
	uint64_t ray_result_bytes = 0;
	if (!_checked_mul(probe_count, p_settings.rays_per_probe, ray_count) ||
			!_checked_mul(ray_count, DDGI_RAY_RESULT_STRIDE, ray_result_bytes)) {
		return fail("ray-result size overflow");
	}

	uint64_t probe_state_bytes = 0;
	if (!_checked_mul(probe_count, DDGI_PROBE_STATE_STRIDE, probe_state_bytes)) {
		return fail("probe-state size overflow");
	}

	const uint64_t irradiance_tile_size = DDGI_IRRADIANCE_TEXELS + 2 * DDGI_ATLAS_BORDER;
	const uint64_t distance_tile_size = DDGI_DISTANCE_TEXELS + 2 * DDGI_ATLAS_BORDER;
	uint64_t irradiance_width = 0;
	uint64_t irradiance_height = 0;
	uint64_t distance_width = 0;
	uint64_t distance_height = 0;
	if (!_checked_mul(count_x, irradiance_tile_size, irradiance_width) ||
			!_checked_mul(count_y, irradiance_tile_size, irradiance_height) ||
			!_checked_mul(count_x, distance_tile_size, distance_width) ||
			!_checked_mul(count_y, distance_tile_size, distance_height)) {
		return fail("probe-atlas dimension overflow");
	}

	if (irradiance_width > UINT32_MAX ||
			irradiance_height > UINT32_MAX ||
			distance_width > UINT32_MAX ||
			distance_height > UINT32_MAX ||
			count_z > UINT32_MAX) {
		return fail("probe-atlas dimensions exceed 32-bit limits");
	}

	RD *rd = RD::get_singleton();
	if (rd) {
		const uint64_t max_texture_size = rd->limit_get(RD::LIMIT_MAX_TEXTURE_SIZE_2D);
		const uint64_t max_array_layers = rd->limit_get(RD::LIMIT_MAX_TEXTURE_ARRAY_LAYERS);
		if (irradiance_width > max_texture_size ||
				irradiance_height > max_texture_size ||
				distance_width > max_texture_size ||
				distance_height > max_texture_size ||
				count_z > max_array_layers) {
			return fail("probe-atlas dimensions exceed device limits");
		}

		const BitField<RD::TextureUsageBits> atlas_usage =
				RD::TEXTURE_USAGE_SAMPLING_BIT |
				RD::TEXTURE_USAGE_STORAGE_BIT |
				RD::TEXTURE_USAGE_CAN_COPY_TO_BIT;
		if (!rd->texture_is_format_supported_for_usage(RD::DATA_FORMAT_R16G16B16A16_SFLOAT, atlas_usage) ||
				!rd->texture_is_format_supported_for_usage(RD::DATA_FORMAT_R16G16_SFLOAT, atlas_usage)) {
			return fail("required floating-point probe-atlas formats are unsupported");
		}
	}

	uint64_t irradiance_texels = 0;
	uint64_t distance_texels = 0;
	uint64_t irradiance_bytes = 0;
	uint64_t distance_bytes = 0;
	if (!_checked_mul(irradiance_width, irradiance_height, irradiance_texels) ||
			!_checked_mul(irradiance_texels, count_z, irradiance_texels) ||
			!_checked_mul(irradiance_texels, 8, irradiance_bytes) ||
			!_checked_mul(distance_width, distance_height, distance_texels) ||
			!_checked_mul(distance_texels, count_z, distance_texels) ||
			!_checked_mul(distance_texels, 4, distance_bytes)) {
		return fail("probe-atlas byte-size overflow");
	}

	if (ray_result_bytes > DDGI_MAX_BUFFER_BYTES ||
			probe_state_bytes > DDGI_MAX_BUFFER_BYTES ||
			irradiance_bytes > DDGI_MAX_ATLAS_BYTES ||
			distance_bytes > DDGI_MAX_ATLAS_BYTES ||
			ray_result_bytes > UINT32_MAX ||
			probe_state_bytes > UINT32_MAX) {
		return fail("a DDGI allocation exceeds its safe per-resource budget");
	}

	uint64_t irradiance_pair_bytes = 0;
	uint64_t distance_pair_bytes = 0;
	uint64_t total_bytes = sizeof(DDGIGridData);
	if (!_checked_mul(irradiance_bytes, 2, irradiance_pair_bytes) ||
			!_checked_mul(distance_bytes, 2, distance_pair_bytes) ||
			!_checked_add(total_bytes, ray_result_bytes, total_bytes) ||
			!_checked_add(total_bytes, probe_state_bytes, total_bytes) ||
			!_checked_add(total_bytes, irradiance_pair_bytes, total_bytes) ||
			!_checked_add(total_bytes, distance_pair_bytes, total_bytes)) {
		return fail("total DDGI resource size overflow");
	}
	if (total_bytes > DDGI_MAX_TOTAL_BYTES) {
		return fail("DDGI resources exceed the 512 MiB per-viewport safety budget");
	}

	r_layout.irradiance_width = uint32_t(irradiance_width);
	r_layout.irradiance_height = uint32_t(irradiance_height);
	r_layout.distance_width = uint32_t(distance_width);
	r_layout.distance_height = uint32_t(distance_height);
	r_layout.layers = uint32_t(count_z);
	r_layout.ray_result_bytes = uint32_t(ray_result_bytes);
	r_layout.probe_state_bytes = uint32_t(probe_state_bytes);
	return true;
}

void _free_resource(RID &r_resource) {
	if (r_resource.is_valid()) {
		RD::get_singleton()->free_rid(r_resource);
		r_resource = RID();
	}
}

RID _create_clear_atlas(RD::DataFormat p_format, uint32_t p_width, uint32_t p_height, uint32_t p_layers, const String &p_name) {
	RD::TextureFormat texture_format;
	texture_format.format = p_format;
	texture_format.width = p_width;
	texture_format.height = p_height;
	texture_format.depth = 1;
	texture_format.array_layers = p_layers;
	texture_format.mipmaps = 1;
	texture_format.texture_type = RD::TEXTURE_TYPE_2D_ARRAY;
	texture_format.usage_bits =
			RD::TEXTURE_USAGE_SAMPLING_BIT |
			RD::TEXTURE_USAGE_STORAGE_BIT |
			RD::TEXTURE_USAGE_CAN_COPY_TO_BIT;

	RID texture = RD::get_singleton()->texture_create(texture_format, RD::TextureView());
	if (texture.is_null()) {
		return RID();
	}
	RD::get_singleton()->set_resource_name(texture, p_name);
	if (RD::get_singleton()->texture_clear(texture, Color(0, 0, 0, 0), 0, 1, 0, p_layers) != OK) {
		RD::get_singleton()->free_rid(texture);
		return RID();
	}
	return texture;
}

bool _is_gi_output_compatible(const Ref<RenderSceneBuffersRD> &p_render_buffers, const StringName &p_name) {
	if (!p_render_buffers->has_texture(RB_SCOPE_GI, p_name)) {
		return false;
	}
	if (!p_render_buffers->get_texture(RB_SCOPE_GI, p_name).is_valid()) {
		return false;
	}

	const RD::TextureFormat texture_format = p_render_buffers->get_texture_format(RB_SCOPE_GI, p_name);
	const Size2i internal_size = p_render_buffers->get_internal_size();
	const uint32_t required_usage =
			RD::TEXTURE_USAGE_SAMPLING_BIT |
			RD::TEXTURE_USAGE_STORAGE_BIT |
			RD::TEXTURE_USAGE_CAN_COPY_TO_BIT;
	return texture_format.format == RD::DATA_FORMAT_R16G16B16A16_SFLOAT &&
			texture_format.texture_type == RD::TEXTURE_TYPE_2D &&
			texture_format.width == uint32_t(internal_size.x) &&
			texture_format.height == uint32_t(internal_size.y) &&
			texture_format.depth == 1 &&
			texture_format.array_layers == 1 &&
			texture_format.mipmaps == 1 &&
			texture_format.samples == RD::TEXTURE_SAMPLES_1 &&
			(texture_format.usage_bits & required_usage) == required_usage;
}

bool _is_ddgi_gbuffer_compatible(const Ref<RenderSceneBuffersRD> &p_render_buffers, const StringName &p_name) {
	if (!p_render_buffers->has_texture(RB_SCOPE_DDGI, p_name)) {
		return false;
	}
	if (!p_render_buffers->get_texture(RB_SCOPE_DDGI, p_name).is_valid()) {
		return false;
	}

	const RD::TextureFormat texture_format = p_render_buffers->get_texture_format(RB_SCOPE_DDGI, p_name);
	const Size2i internal_size = p_render_buffers->get_internal_size();
	const uint32_t required_usage =
			RD::TEXTURE_USAGE_SAMPLING_BIT |
			RD::TEXTURE_USAGE_STORAGE_BIT |
			RD::TEXTURE_USAGE_CAN_COPY_TO_BIT;

	const bool format_compatible =
			((p_name == RB_TEX_GBUFFER_A || p_name == RB_TEX_GBUFFER_DEBUG) &&
					texture_format.format == RD::DATA_FORMAT_R8G8B8A8_UNORM) ||
			((p_name == RB_TEX_GBUFFER_B || p_name == RB_TEX_GBUFFER_C || p_name == RB_TEX_GBUFFER_D) &&
					texture_format.format == RD::DATA_FORMAT_R32G32B32A32_SFLOAT);
	return format_compatible &&
			texture_format.texture_type == RD::TEXTURE_TYPE_2D &&
			texture_format.width == uint32_t(internal_size.x) &&
			texture_format.height == uint32_t(internal_size.y) &&
			texture_format.depth == 1 &&
			texture_format.array_layers == 1 &&
			texture_format.mipmaps == 1 &&
			texture_format.samples == RD::TEXTURE_SAMPLES_1 &&
			(texture_format.usage_bits & required_usage) == required_usage;
}

} // namespace

bool DDGISettings::is_valid() const {
	return probe_count.x > 0 &&
			probe_count.y > 0 &&
			probe_count.z > 0 &&
			probe_spacing.is_finite() &&
			probe_spacing.x > 0.0f &&
			probe_spacing.y > 0.0f &&
			probe_spacing.z > 0.0f &&
			rays_per_probe > 0 &&
			Math::is_finite(max_ray_distance) &&
			max_ray_distance > 0.0f &&
			Math::is_finite(hysteresis) &&
			Math::is_finite(normal_bias) &&
			Math::is_finite(view_bias) &&
			Math::is_finite(energy);
}

bool DDGISettings::has_same_structure(const DDGISettings &p_other) const {
	return probe_count == p_other.probe_count &&
			probe_spacing == p_other.probe_spacing &&
			rays_per_probe == p_other.rays_per_probe;
}

void DDGIState::prepare_frame(const DDGISettings &p_settings, RID p_environment, RID p_scenario, const DDGIFrameGrid &p_grid, uint64_t p_scene_pass) {
	const bool had_valid_grid = current_grid.valid;
	const bool same_environment = environment == p_environment;
	const bool same_scenario = scenario == p_scenario;
	const bool structure_changed = had_valid_grid && !settings.has_same_structure(p_settings);
	const bool sampling_semantics_changed = had_valid_grid &&
			(settings.max_ray_distance != p_settings.max_ray_distance ||
					settings.normal_bias != p_settings.normal_bias ||
					settings.view_bias != p_settings.view_bias ||
					settings.read_sky != p_settings.read_sky);

	DDGIFrameGrid next_grid = p_grid;
	bool scroll_requires_reset = false;
	if (had_valid_grid && !structure_changed && same_environment && same_scenario) {
		const Vector3 cell_delta = (next_grid.origin - current_grid.origin) / p_settings.probe_spacing;
		const bool representable = cell_delta.is_finite() &&
				double(cell_delta.x) >= double(INT32_MIN) && double(cell_delta.x) <= double(INT32_MAX) &&
				double(cell_delta.y) >= double(INT32_MIN) && double(cell_delta.y) <= double(INT32_MAX) &&
				double(cell_delta.z) >= double(INT32_MIN) && double(cell_delta.z) <= double(INT32_MAX);
		if (representable) {
			const Vector3i integer_delta(
					int32_t(Math::round(cell_delta.x)),
					int32_t(Math::round(cell_delta.y)),
					int32_t(Math::round(cell_delta.z)));
			const bool integer_scroll =
					Math::is_equal_approx(cell_delta.x, real_t(integer_delta.x)) &&
					Math::is_equal_approx(cell_delta.y, real_t(integer_delta.y)) &&
					Math::is_equal_approx(cell_delta.z, real_t(integer_delta.z)) && 
					Math::abs(integer_delta.x) < p_grid.probe_count.x && Math::abs(integer_delta.y) < p_grid.probe_count.y && Math::abs(integer_delta.z) < p_grid.probe_count.z;
			if (integer_scroll) {
				next_grid.scroll_delta = integer_delta;
				scroll_requires_reset = false;
			} else {
				scroll_requires_reset = true;
			}
		} else {
			scroll_requires_reset = true;
		}
	}

	if (structure_changed) {
		_free_resources();
	}

	// Probe count/spacing/rays-per-probe are structural and rebuild resources.
	// Max distance, sky participation, and sampling biases change the meaning of
	// retained samples and therefore reset history. Hysteresis only controls
	// future blending and energy is applied at resolve, so both update in place.
	history_reset = history_reset ||
			!had_valid_grid ||
			!same_environment ||
			!same_scenario ||
			structure_changed ||
			sampling_semantics_changed ||
			scroll_requires_reset;

	previous_grid = current_grid;
	current_grid = next_grid;
	settings = p_settings;
	environment = p_environment;
	scenario = p_scenario;
	prepared_scene_pass = p_scene_pass;
	committed_scene_pass = 0;
}

bool DDGIState::ensure_probe_resources() {
	if (resources_ready) {
		return true;
	}

	DDGIResourceLayout layout;
	String error;
	if (!_calculate_resource_layout(settings, layout, &error)) {
		if (!resource_failure_reported) {
			ERR_PRINT(vformat("Cannot allocate DDGI probe resources: %s.", error));
			resource_failure_reported = true;
		}
		return false;
	}

	const bool failure_was_reported = resource_failure_reported;
	_free_resources();
	resource_failure_reported = failure_was_reported;

	grid_uniform_buffer = RD::get_singleton()->uniform_buffer_create(sizeof(DDGIGridData));
	if (grid_uniform_buffer.is_valid()) {
		RD::get_singleton()->set_resource_name(grid_uniform_buffer, "DDGI Grid Parameters");
	}

	ray_data = RD::get_singleton()->storage_buffer_create(layout.ray_result_bytes);
	if (ray_data.is_valid()) {
		RD::get_singleton()->set_resource_name(ray_data, "DDGI Ray Results");
		if (RD::get_singleton()->buffer_clear(ray_data, 0, layout.ray_result_bytes) != OK) {
			_free_resource(ray_data);
		}
	}

	probe_state_buffer = RD::get_singleton()->storage_buffer_create(layout.probe_state_bytes);
	if (probe_state_buffer.is_valid()) {
		RD::get_singleton()->set_resource_name(probe_state_buffer, "DDGI Probe State");
		if (RD::get_singleton()->buffer_clear(probe_state_buffer, 0, layout.probe_state_bytes) != OK) {
			_free_resource(probe_state_buffer);
		}
	}

	irradiance_atlas[0] = _create_clear_atlas(
			RD::DATA_FORMAT_R16G16B16A16_SFLOAT,
			layout.irradiance_width,
			layout.irradiance_height,
			layout.layers,
			"DDGI Irradiance Atlas A");
	irradiance_atlas[1] = _create_clear_atlas(
			RD::DATA_FORMAT_R16G16B16A16_SFLOAT,
			layout.irradiance_width,
			layout.irradiance_height,
			layout.layers,
			"DDGI Irradiance Atlas B");
	distance_atlas[0] = _create_clear_atlas(
			RD::DATA_FORMAT_R16G16_SFLOAT,
			layout.distance_width,
			layout.distance_height,
			layout.layers,
			"DDGI Distance Atlas A");
	distance_atlas[1] = _create_clear_atlas(
			RD::DATA_FORMAT_R16G16_SFLOAT,
			layout.distance_width,
			layout.distance_height,
			layout.layers,
			"DDGI Distance Atlas B");

	resources_ready = grid_uniform_buffer.is_valid() &&
			ray_data.is_valid() &&
			probe_state_buffer.is_valid() &&
			irradiance_atlas[0].is_valid() &&
			irradiance_atlas[1].is_valid() &&
			distance_atlas[0].is_valid() &&
			distance_atlas[1].is_valid();
	if (!resources_ready) {
		const bool should_report_failure = !resource_failure_reported;
		_free_resources();
		resource_failure_reported = true;
		if (should_report_failure) {
			ERR_PRINT("Cannot allocate one or more DDGI probe resources; DDGI will be disabled for this frame.");
		}
		return false;
	}

	current_atlas_index = 0;
	snapshot_generation = 0;
	history_reset = true;
	resource_failure_reported = false;
	return true;
}

void DDGIState::commit_frame(uint64_t p_snapshot_generation) {
	ERR_FAIL_COND(!resources_ready);
	ERR_FAIL_COND(p_snapshot_generation == 0);

	snapshot_generation = p_snapshot_generation;
	committed_scene_pass = prepared_scene_pass;
	current_atlas_index = 1u - current_atlas_index;
	history_reset = false;
}

bool DDGIState::is_configured_for(const RenderSceneBuffersRD *p_render_buffers) const {
	return render_buffers == p_render_buffers;
}

bool DDGIState::is_prepared_for_scene_pass(uint64_t p_scene_pass) const {
	return resources_ready && current_grid.valid && prepared_scene_pass != 0 && prepared_scene_pass == p_scene_pass;
}

bool DDGIState::is_committed_for_scene_pass(uint64_t p_scene_pass) const {
	return is_prepared_for_scene_pass(p_scene_pass) && committed_scene_pass == p_scene_pass;
}

void DDGIState::configure(RenderSceneBuffersRD *p_render_buffers) {
	ERR_FAIL_NULL(p_render_buffers);

	if (render_buffers == p_render_buffers) {
		return;
	}

	free_data();
	render_buffers = p_render_buffers;
}

void DDGIState::_free_resources() {
	_free_resource(grid_uniform_buffer);
	_free_resource(ray_data);
	_free_resource(probe_state_buffer);

	for (uint32_t i = 0; i < 2; i++) {
		_free_resource(irradiance_atlas[i]);
		_free_resource(distance_atlas[i]);
	}

	current_atlas_index = 0;
	committed_scene_pass = 0;
	snapshot_generation = 0;
	resources_ready = false;
	resource_failure_reported = false;
}

void DDGIState::free_data() {
	_free_resources();
	settings = DDGISettings();
	environment = RID();
	scenario = RID();
	current_grid = DDGIFrameGrid();
	previous_grid = DDGIFrameGrid();
	prepared_scene_pass = 0;
	committed_scene_pass = 0;
	snapshot_generation = 0;
	current_atlas_index = 0;
	history_reset = true;
	resources_ready = false;
	render_buffers = nullptr;
}

DDGIState::~DDGIState() {
	free_data();
}

DDGIFrameGrid DDGI::_build_frame_grid(const DDGISettings &p_settings, const Vector3 &p_camera_position) {
	DDGIFrameGrid grid;
	grid.probe_count = p_settings.probe_count;
	grid.probe_spacing = p_settings.probe_spacing;

	const Vector3 grid_size(
			real_t(p_settings.probe_count.x - 1) * p_settings.probe_spacing.x,
			real_t(p_settings.probe_count.y - 1) * p_settings.probe_spacing.y,
			real_t(p_settings.probe_count.z - 1) * p_settings.probe_spacing.z);
	if (!grid_size.is_finite()) {
		return grid;
	}
	const Vector3 centered_origin = p_camera_position - grid_size * 0.5f;
	if (!centered_origin.is_finite()) {
		return grid;
	}

	grid.origin = centered_origin.snapped(p_settings.probe_spacing); //对齐步长
	grid.probe_bounds = AABB(grid.origin, grid_size);

	const float max_spacing = MAX(p_settings.probe_spacing.x, MAX(p_settings.probe_spacing.y, p_settings.probe_spacing.z));
	const float relocation_allowance = max_spacing * 0.5f;
	grid.expanded_bounds = grid.probe_bounds.grow(p_settings.max_ray_distance + relocation_allowance);
	grid.valid = grid.origin.is_finite() && grid.probe_bounds.is_finite() && grid.expanded_bounds.is_finite();
	return grid;
}

DDGI::DDGI(bool p_use_radiance_octmap_array) {
	use_radiance_octmap_array = p_use_radiance_octmap_array;

	Vector<String> shader_modes;
	shader_modes.push_back("");

	String gbuffer_defines;
	if (p_use_radiance_octmap_array) {
		gbuffer_defines += "\n#define USE_RADIANCE_OCTMAP_ARRAY\n";
	}
	gbuffer_shader.initialize(shader_modes, gbuffer_defines);
	gbuffer_shader_version = gbuffer_shader.version_create();
	const RID gbuffer_shader_rid = gbuffer_shader.version_get_shader(gbuffer_shader_version, 0);
	if (gbuffer_shader_rid.is_valid()) {
		const RD::PipelineShader pipeline_shader = { gbuffer_shader_rid, {} };
		const RD::HitGroup empty_hit_group;
		gbuffer_pipeline = RD::get_singleton()->raytracing_pipeline_create(
				{ &pipeline_shader, 1 },
				{},
				{ &empty_hit_group, 1 },
				1);
	}
	if (gbuffer_pipeline.is_valid()) {
		gbuffer_hit_sbt = RD::get_singleton()->hit_sbt_create(gbuffer_pipeline, 1);
		if (gbuffer_hit_sbt.is_valid()) {
			const RD::HitShaderBindingTableRange range = RD::get_singleton()->hit_sbt_range_alloc(gbuffer_hit_sbt, 1);
			const uint32_t empty_hit_group_index = 0;
			if (!range ||
					RD::get_singleton()->hit_sbt_range_update(gbuffer_hit_sbt, range, 0, { &empty_hit_group_index, 1 }) != OK) {
				RD::get_singleton()->free_rid(gbuffer_hit_sbt);
				gbuffer_hit_sbt = RID();
			}
		}
	}

	probe_update_shader.initialize(shader_modes, gbuffer_defines);
	probe_update_shader_version = probe_update_shader.version_create();
	const RID probe_update_shader_rid =
			probe_update_shader.version_get_shader(probe_update_shader_version, 0);
	if (probe_update_shader_rid.is_valid()) {
		const RD::PipelineShader pipeline_shader = { probe_update_shader_rid, {} };
		const RD::HitGroup empty_hit_group;
		probe_update_pipeline = RD::get_singleton()->raytracing_pipeline_create(
				{ &pipeline_shader, 1 },
				{},
				{ &empty_hit_group, 1 },
				1);
	}
	if (probe_update_pipeline.is_valid()) {
		probe_update_hit_sbt = RD::get_singleton()->hit_sbt_create(probe_update_pipeline, 1);
		if (probe_update_hit_sbt.is_valid()) {
			const RD::HitShaderBindingTableRange range =
					RD::get_singleton()->hit_sbt_range_alloc(probe_update_hit_sbt, 1);
			const uint32_t empty_hit_group_index = 0;
			if (!range ||
					RD::get_singleton()->hit_sbt_range_update(
							probe_update_hit_sbt,
							range,
							0,
							{ &empty_hit_group_index, 1 }) != OK) {
				RD::get_singleton()->free_rid(probe_update_hit_sbt);
				probe_update_hit_sbt = RID();
			}
		}
	}

	probe_blend_shader.initialize(shader_modes);
	probe_blend_shader_version = probe_blend_shader.version_create();
	probe_blend_pipeline = RD::get_singleton()->compute_pipeline_create(
			probe_blend_shader.version_get_shader(probe_blend_shader_version, 0));

	resolve_shader.initialize(shader_modes);
	resolve_shader_version = resolve_shader.version_create();
	resolve_pipeline = RD::get_singleton()->compute_pipeline_create(resolve_shader.version_get_shader(resolve_shader_version, 0));
}

DDGI::~DDGI() {
	if (gbuffer_hit_sbt.is_valid()) {
		RD::get_singleton()->free_rid(gbuffer_hit_sbt);
		gbuffer_hit_sbt = RID();
	}
	if (gbuffer_pipeline.is_valid()) {
		RD::get_singleton()->free_rid(gbuffer_pipeline);
		gbuffer_pipeline = RID();
	}
	if (gbuffer_shader_version.is_valid()) {
		gbuffer_shader.version_free(gbuffer_shader_version);
		gbuffer_shader_version = RID();
	}

	if (probe_update_hit_sbt.is_valid()) {
		RD::get_singleton()->free_rid(probe_update_hit_sbt);
		probe_update_hit_sbt = RID();
	}
	if (probe_update_pipeline.is_valid()) {
		RD::get_singleton()->free_rid(probe_update_pipeline);
		probe_update_pipeline = RID();
	}
	if (probe_update_shader_version.is_valid()) {
		probe_update_shader.version_free(probe_update_shader_version);
		probe_update_shader_version = RID();
	}

	if (probe_blend_pipeline.is_valid()) {
		RD::get_singleton()->free_rid(probe_blend_pipeline);
		probe_blend_pipeline = RID();
	}
	if (probe_blend_shader_version.is_valid()) {
		probe_blend_shader.version_free(probe_blend_shader_version);
		probe_blend_shader_version = RID();
	}

	if (resolve_pipeline.is_valid()) {
		RD::get_singleton()->free_rid(resolve_pipeline);
		resolve_pipeline = RID();
	}
	resolve_shader.version_free(resolve_shader_version);
}

bool DDGI::prepare_frame(const Ref<RenderSceneBuffersRD> &p_render_buffers, const DDGISettings &p_settings, RID p_environment, RID p_scenario, const Vector3 &p_camera_position, uint64_t p_scene_pass, AABB &r_expanded_bounds) {
	r_expanded_bounds = AABB();

	ERR_FAIL_COND_V(p_render_buffers.is_null(), false);

	Ref<DDGIState> state;
	if (p_render_buffers->has_custom_data(RB_SCOPE_DDGI)) {
		state = p_render_buffers->get_custom_data(RB_SCOPE_DDGI);
	}
	auto fail_preparation = [&]() {
		if (state.is_valid()) {
			state->free_data();
		}
		if (p_render_buffers->has_custom_data(RB_SCOPE_DDGI)) {
			p_render_buffers->set_custom_data(RB_SCOPE_DDGI, Ref<RenderBufferCustomDataRD>()); //相当于清空数据
		}

		// Discard DDGI-owned G-buffers so later frames cannot reuse partial data.
		p_render_buffers->clear_context(RB_SCOPE_DDGI); //这里会吧纹理一起删掉
		// A fallback GI backend runs immediately after this pre-cull hook. Do
		// not leave DDGI-sized outputs for it to mistake for valid history.
		p_render_buffers->clear_context(RB_SCOPE_GI);
		return false;
	};

	if (!p_settings.is_valid() || !p_camera_position.is_finite()) {
		return fail_preparation();
	}
	if (!gbuffer_shader_version.is_valid() || !gbuffer_pipeline.is_valid() || !gbuffer_hit_sbt.is_valid() ||
			!resolve_shader_version.is_valid() || !resolve_pipeline.is_valid()) {
		ERR_PRINT_ONCE("DDGI shader initialization failed.");
		return fail_preparation();
	}

	DDGIResourceLayout layout;
	String layout_error;
	if (!_calculate_resource_layout(p_settings, layout, &layout_error)) {
		ERR_PRINT_ONCE(vformat("DDGI settings were rejected before allocation: %s.", layout_error));
		return fail_preparation();
	}

	const bool entering_ddgi = state.is_null() ||
			!state->is_configured_for(p_render_buffers.ptr()) ||
			!state->current_grid.valid ||
			state->environment != p_environment ||
			state->scenario != p_scenario;
	const DDGIFrameGrid grid = _build_frame_grid(p_settings, p_camera_position);
	if (!grid.valid) {
		return fail_preparation();
	}

	if (state.is_null()) {
		state.instantiate();
		state->configure(p_render_buffers.ptr());
		p_render_buffers->set_custom_data(RB_SCOPE_DDGI, state);
	} else if (!state->is_configured_for(p_render_buffers.ptr())) {
		state->configure(p_render_buffers.ptr());
	}

	state->prepare_frame(p_settings, p_environment, p_scenario, grid, p_scene_pass);
	if (!state->ensure_probe_resources() ||
			!ensure_gi_outputs(p_render_buffers, entering_ddgi) || !ensure_ddgi_gbuffer_textures(p_render_buffers, entering_ddgi)) {
		return fail_preparation();
	}

	r_expanded_bounds = state->current_grid.expanded_bounds;
	return true;
}

bool DDGI::clear_state(const Ref<RenderSceneBuffersRD> &p_render_buffers) {
	if (p_render_buffers.is_null()) {
		return false;
	}

	p_render_buffers->clear_context(RB_SCOPE_DDGI);
	if (!p_render_buffers->has_custom_data(RB_SCOPE_DDGI)) {
		return false;
	}

	Ref<DDGIState> state = p_render_buffers->get_custom_data(RB_SCOPE_DDGI);
	if (state.is_valid()) {
		state->free_data();
	}
	p_render_buffers->set_custom_data(RB_SCOPE_DDGI, Ref<RenderBufferCustomDataRD>());
	return true;
}

bool DDGI::ensure_ddgi_gbuffer_textures(const Ref<RenderSceneBuffersRD> &p_render_buffers, bool p_clear_existing) {
	ERR_FAIL_COND_V(p_render_buffers.is_null(), false);
	ERR_FAIL_COND_V(p_render_buffers->get_internal_size().x <= 0 || p_render_buffers->get_internal_size().y <= 0, false);
	ERR_FAIL_COND_V(p_render_buffers->get_view_count() != 1, false);

	const uint32_t usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT;

	if (p_clear_existing ||
			!_is_ddgi_gbuffer_compatible(p_render_buffers, RB_TEX_GBUFFER_A) ||
			!_is_ddgi_gbuffer_compatible(p_render_buffers, RB_TEX_GBUFFER_B) ||
			!_is_ddgi_gbuffer_compatible(p_render_buffers, RB_TEX_GBUFFER_C) ||
			!_is_ddgi_gbuffer_compatible(p_render_buffers, RB_TEX_GBUFFER_D) ||
			!_is_ddgi_gbuffer_compatible(p_render_buffers, RB_TEX_GBUFFER_DEBUG)) {
		p_render_buffers->clear_context(RB_SCOPE_DDGI);
	}

	if (!p_render_buffers->has_texture(RB_SCOPE_DDGI, RB_TEX_GBUFFER_A)) {
		p_render_buffers->create_texture(RB_SCOPE_DDGI, RB_TEX_GBUFFER_A, RD::DATA_FORMAT_R8G8B8A8_UNORM, usage_bits, RD::TEXTURE_SAMPLES_1);
	}
	if (!p_render_buffers->has_texture(RB_SCOPE_DDGI, RB_TEX_GBUFFER_B)) {
		p_render_buffers->create_texture(RB_SCOPE_DDGI, RB_TEX_GBUFFER_B, RD::DATA_FORMAT_R32G32B32A32_SFLOAT, usage_bits, RD::TEXTURE_SAMPLES_1);
	}
	if (!p_render_buffers->has_texture(RB_SCOPE_DDGI, RB_TEX_GBUFFER_C)) {
		p_render_buffers->create_texture(RB_SCOPE_DDGI, RB_TEX_GBUFFER_C, RD::DATA_FORMAT_R32G32B32A32_SFLOAT, usage_bits, RD::TEXTURE_SAMPLES_1);
	}
	if (!p_render_buffers->has_texture(RB_SCOPE_DDGI, RB_TEX_GBUFFER_D)) {
		p_render_buffers->create_texture(RB_SCOPE_DDGI, RB_TEX_GBUFFER_D, RD::DATA_FORMAT_R32G32B32A32_SFLOAT, usage_bits, RD::TEXTURE_SAMPLES_1);
	}
	if (!p_render_buffers->has_texture(RB_SCOPE_DDGI, RB_TEX_GBUFFER_DEBUG)) {
		p_render_buffers->create_texture(RB_SCOPE_DDGI, RB_TEX_GBUFFER_DEBUG, RD::DATA_FORMAT_R8G8B8A8_UNORM, usage_bits, RD::TEXTURE_SAMPLES_1);
	}

	if (!_is_ddgi_gbuffer_compatible(p_render_buffers, RB_TEX_GBUFFER_A) ||
			!_is_ddgi_gbuffer_compatible(p_render_buffers, RB_TEX_GBUFFER_B) ||
			!_is_ddgi_gbuffer_compatible(p_render_buffers, RB_TEX_GBUFFER_C) ||
			!_is_ddgi_gbuffer_compatible(p_render_buffers, RB_TEX_GBUFFER_D) ||
			!_is_ddgi_gbuffer_compatible(p_render_buffers, RB_TEX_GBUFFER_DEBUG)) {
		p_render_buffers->clear_context(RB_SCOPE_DDGI);
		return false;
	}

	return true;
}

bool DDGI::clear_ddgi_gbuffer_textures(const Ref<RenderSceneBuffersRD> &p_render_buffers) {
	if (p_render_buffers.is_null()) {
		return false;
	}

	if (!_is_ddgi_gbuffer_compatible(p_render_buffers, RB_TEX_GBUFFER_A) ||
			!_is_ddgi_gbuffer_compatible(p_render_buffers, RB_TEX_GBUFFER_B) ||
			!_is_ddgi_gbuffer_compatible(p_render_buffers, RB_TEX_GBUFFER_C) ||
			!_is_ddgi_gbuffer_compatible(p_render_buffers, RB_TEX_GBUFFER_D) ||
			!_is_ddgi_gbuffer_compatible(p_render_buffers, RB_TEX_GBUFFER_DEBUG)) {
		p_render_buffers->clear_context(RB_SCOPE_DDGI);
		return true;
	}

	if (RD::get_singleton()->texture_clear(p_render_buffers->get_texture(RB_SCOPE_DDGI, RB_TEX_GBUFFER_A), Color(0, 0, 0, 0), 0, 1, 0, 1) != OK ||
			RD::get_singleton()->texture_clear(p_render_buffers->get_texture(RB_SCOPE_DDGI, RB_TEX_GBUFFER_B), Color(0, 0, 0, 0), 0, 1, 0, 1) != OK ||
			RD::get_singleton()->texture_clear(p_render_buffers->get_texture(RB_SCOPE_DDGI, RB_TEX_GBUFFER_C), Color(0, 0, 0, 0), 0, 1, 0, 1) != OK ||
			RD::get_singleton()->texture_clear(p_render_buffers->get_texture(RB_SCOPE_DDGI, RB_TEX_GBUFFER_D), Color(0, 0, 0, 0), 0, 1, 0, 1) != OK ||
			RD::get_singleton()->texture_clear(p_render_buffers->get_texture(RB_SCOPE_DDGI, RB_TEX_GBUFFER_DEBUG), Color(0, 0, 0, 0), 0, 1, 0, 1) != OK) {
		p_render_buffers->clear_context(RB_SCOPE_DDGI);
		return false;
	}
	return true;
}

bool DDGI::ensure_gi_outputs(const Ref<RenderSceneBuffersRD> &p_render_buffers, bool p_clear_existing) {
	ERR_FAIL_COND_V(p_render_buffers.is_null(), false);
	ERR_FAIL_COND_V(p_render_buffers->get_internal_size().x <= 0 || p_render_buffers->get_internal_size().y <= 0, false);
	ERR_FAIL_COND_V(p_render_buffers->get_view_count() != 1, false);
	ERR_FAIL_COND_V(!p_render_buffers->has_custom_data(RB_SCOPE_GI), false);

	Ref<GI::RenderBuffersGI> rbgi = p_render_buffers->get_custom_data(RB_SCOPE_GI);
	ERR_FAIL_COND_V(rbgi.is_null(), false);

	const bool outputs_compatible =
			_is_gi_output_compatible(p_render_buffers, RB_TEX_AMBIENT) &&
			_is_gi_output_compatible(p_render_buffers, RB_TEX_REFLECTION);
	if (p_clear_existing || !outputs_compatible || rbgi->using_half_size_gi) {
		p_render_buffers->clear_context(RB_SCOPE_GI);
	}
	rbgi->using_half_size_gi = false;

	if (!p_render_buffers->has_texture(RB_SCOPE_GI, RB_TEX_AMBIENT)) {
		const uint32_t usage_bits =
				RD::TEXTURE_USAGE_SAMPLING_BIT |
				RD::TEXTURE_USAGE_STORAGE_BIT |
				RD::TEXTURE_USAGE_CAN_COPY_TO_BIT;
		const Size2i internal_size = p_render_buffers->get_internal_size();
		p_render_buffers->create_texture(RB_SCOPE_GI, RB_TEX_AMBIENT, RD::DATA_FORMAT_R16G16B16A16_SFLOAT, usage_bits, RD::TEXTURE_SAMPLES_1, internal_size, 1);
		p_render_buffers->create_texture(RB_SCOPE_GI, RB_TEX_REFLECTION, RD::DATA_FORMAT_R16G16B16A16_SFLOAT, usage_bits, RD::TEXTURE_SAMPLES_1, internal_size, 1);

		if (!_is_gi_output_compatible(p_render_buffers, RB_TEX_AMBIENT) ||
				!_is_gi_output_compatible(p_render_buffers, RB_TEX_REFLECTION)) {
			p_render_buffers->clear_context(RB_SCOPE_GI);
			return false;
		}

		if (RD::get_singleton()->texture_clear(p_render_buffers->get_texture(RB_SCOPE_GI, RB_TEX_AMBIENT), Color(0, 0, 0, 0), 0, 1, 0, 1) != OK ||
				RD::get_singleton()->texture_clear(p_render_buffers->get_texture(RB_SCOPE_GI, RB_TEX_REFLECTION), Color(0, 0, 0, 0), 0, 1, 0, 1) != OK) {
			p_render_buffers->clear_context(RB_SCOPE_GI);
			return false;
		}
	}

	return true;
}

bool DDGI::clear_gi_outputs(const Ref<RenderSceneBuffersRD> &p_render_buffers) {
	if (p_render_buffers.is_null()) {
		return false;
	}

	if (!_is_gi_output_compatible(p_render_buffers, RB_TEX_AMBIENT) ||
			!_is_gi_output_compatible(p_render_buffers, RB_TEX_REFLECTION)) {
		p_render_buffers->clear_context(RB_SCOPE_GI);
		return true;
	}

	if (RD::get_singleton()->texture_clear(p_render_buffers->get_texture(RB_SCOPE_GI, RB_TEX_AMBIENT), Color(0, 0, 0, 0), 0, 1, 0, 1) != OK ||
			RD::get_singleton()->texture_clear(p_render_buffers->get_texture(RB_SCOPE_GI, RB_TEX_REFLECTION), Color(0, 0, 0, 0), 0, 1, 0, 1) != OK) {
		p_render_buffers->clear_context(RB_SCOPE_GI);
		return false;
	}
	return true;
}

bool DDGI::update_ddgi_gbuffer(
		const Ref<DDGIState> &p_state,
		const RendererSceneRenderImplementation::RTSceneSnapshot &p_snapshot,
		RendererSceneRenderImplementation::RenderRaytracing &p_rt_service,
		const RenderDataRD *p_render_data,
		RID p_sky_radiance) {
	ERR_FAIL_COND_V(p_state.is_null(), false);
	ERR_FAIL_NULL_V(p_render_data, false);
	ERR_FAIL_COND_V(p_render_data->render_buffers.is_null(), false);
	ERR_FAIL_COND_V(!p_state->is_configured_for(p_render_data->render_buffers.ptr()), false);
	ERR_FAIL_COND_V(!p_state->current_grid.valid || p_state->prepared_scene_pass == 0, false);
	ERR_FAIL_COND_V(!p_snapshot.is_valid(), false);
	ERR_FAIL_COND_V(!p_snapshot.consumers.has_flag(RT_SCENE_CONSUMER_DDGI), false);
	ERR_FAIL_COND_V(!gbuffer_pipeline.is_valid() || !gbuffer_hit_sbt.is_valid(), false);

	Ref<RenderSceneBuffersRD> render_buffers = p_render_data->render_buffers;
	if (!ensure_ddgi_gbuffer_textures(render_buffers)) {
		return false;
	}

	RendererSceneRenderImplementation::RTViewportState *rt_state = p_rt_service.get_viewport_state(p_snapshot);
	ERR_FAIL_NULL_V(rt_state, false);

	RendererSceneRenderImplementation::RT_LightData light_data[RendererSceneRenderImplementation::RT_LIGHTS_MAX] = {};
	const uint32_t light_count = p_rt_service.gather_lights(
			p_render_data,
			light_data,
			RendererSceneRenderImplementation::RT_LIGHTS_MAX);
	const uint32_t light_buffer_size =
			RendererSceneRenderImplementation::RT_LIGHTS_MAX *
			sizeof(RendererSceneRenderImplementation::RT_LightData);
	if (!rt_state->light_buffer.is_valid()) {
		rt_state->light_buffer = RD::get_singleton()->storage_buffer_create(light_buffer_size);
		if (rt_state->light_buffer.is_valid()) {
			RD::get_singleton()->set_resource_name(rt_state->light_buffer, "DDGI Light Buffer");
		}
	}
	if (!rt_state->light_buffer.is_valid() ||
			RD::get_singleton()->buffer_update(rt_state->light_buffer, 0, light_buffer_size, light_data) != OK) {
		return false;
	}

	const RID shader = gbuffer_shader.version_get_shader(gbuffer_shader_version, 0);
	ERR_FAIL_COND_V(!shader.is_valid(), false);

	RendererRD::TextureStorage *texture_storage = RendererRD::TextureStorage::get_singleton();
	ERR_FAIL_NULL_V(texture_storage, false);
	RendererRD::MaterialStorage *material_storage = RendererRD::MaterialStorage::get_singleton();
	ERR_FAIL_NULL_V(material_storage, false);
	RendererRD::MeshStorage *mesh_storage = RendererRD::MeshStorage::get_singleton();
	ERR_FAIL_NULL_V(mesh_storage, false);

	RID sky_radiance = p_state->settings.read_sky ? p_sky_radiance : RID();
	if (!sky_radiance.is_valid()) {
		sky_radiance = texture_storage->texture_rd_get_default(
				use_radiance_octmap_array
						? RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_2D_ARRAY_BLACK
						: RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_BLACK);
	}

	LocalVector<RD::Uniform> uniforms;
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 0, render_buffers->get_texture(RB_SCOPE_DDGI, RB_TEX_GBUFFER_A)));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_ACCELERATION_STRUCTURE, 1, p_snapshot.tlas));

	const RID scene_uniform_buffer = p_render_data->scene_data->get_uniform_buffer();
	//Debug
	//print_line(vformat("directional_light_count: %d", p_render_data->lights->size()));
	ERR_FAIL_COND_V(!scene_uniform_buffer.is_valid(), false);
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 2, scene_uniform_buffer));

	ERR_FAIL_COND_V(
			p_snapshot.instance_count > 0 &&
					(!p_snapshot.geometry_buffer.is_valid() || !p_snapshot.material_buffer.is_valid()),
			false);
	const RID default_storage_buffer = mesh_storage->get_default_rd_storage_buffer();
	uniforms.push_back(RD::Uniform(
			RD::UNIFORM_TYPE_STORAGE_BUFFER,
			3,
			p_snapshot.geometry_buffer.is_valid() ? p_snapshot.geometry_buffer : default_storage_buffer));
	uniforms.push_back(RD::Uniform(
			RD::UNIFORM_TYPE_STORAGE_BUFFER,
			5,
			p_snapshot.material_buffer.is_valid() ? p_snapshot.material_buffer : default_storage_buffer));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 7, sky_radiance));
	uniforms.push_back(RD::Uniform(
			RD::UNIFORM_TYPE_SAMPLER,
			8,
			material_storage->sampler_rd_get_default(
					RSE::CANVAS_ITEM_TEXTURE_FILTER_LINEAR_WITH_MIPMAPS,
					RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED)));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 13, rt_state->light_buffer));
	uniforms.push_back(RD::Uniform(
			RD::UNIFORM_TYPE_SAMPLER,
			24,
			material_storage->sampler_rd_get_default(
					RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST_WITH_MIPMAPS,
					RSE::CANVAS_ITEM_TEXTURE_REPEAT_ENABLED)));
	uniforms.push_back(RD::Uniform(
			RD::UNIFORM_TYPE_SAMPLER,
			25,
			material_storage->sampler_rd_get_default(
					RSE::CANVAS_ITEM_TEXTURE_FILTER_LINEAR_WITH_MIPMAPS,
					RSE::CANVAS_ITEM_TEXTURE_REPEAT_ENABLED)));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 29, render_buffers->get_texture(RB_SCOPE_DDGI, RB_TEX_GBUFFER_B)));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 30, render_buffers->get_texture(RB_SCOPE_DDGI, RB_TEX_GBUFFER_C)));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 31, render_buffers->get_texture(RB_SCOPE_DDGI, RB_TEX_GBUFFER_D)));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 32, render_buffers->get_texture(RB_SCOPE_DDGI, RB_TEX_GBUFFER_DEBUG)));

	UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();
	ERR_FAIL_NULL_V(uniform_set_cache, false);
	const RID uniform_set = uniform_set_cache->get_cache_vec(shader, 0, uniforms);
	ERR_FAIL_COND_V(!uniform_set.is_valid() || !RD::get_singleton()->uniform_set_is_valid(uniform_set), false);

	const RID bindless_set = p_rt_service.finalize_bindless_uniform_set(shader, 1);
	ERR_FAIL_COND_V(!bindless_set.is_valid() || !RD::get_singleton()->uniform_set_is_valid(bindless_set), false);

	GBufferPushConstant push_constant = {};
	push_constant.light_count = light_count;
	push_constant.frame_index = uint32_t(p_state->prepared_scene_pass);
	// push_constant.frame_index = 0; //Debug 排除时阈
	push_constant.normal_bias = p_state->settings.normal_bias;
	push_constant.view_bias = p_state->settings.view_bias;
	RendererEnvironmentStorage *environment_storage = RendererEnvironmentStorage::get_singleton();
	ERR_FAIL_NULL_V(environment_storage, false);
	push_constant.debug_mode = uint32_t(environment_storage->environment_get_ddgi_debug_mode(p_state->environment));

	const Vector3 debug_bounds_min = p_state->current_grid.origin - p_state->settings.probe_spacing * 0.5f;
	const Vector3 debug_bounds_size(
			p_state->settings.probe_spacing.x * p_state->settings.probe_count.x,
			p_state->settings.probe_spacing.y * p_state->settings.probe_count.y,
			p_state->settings.probe_spacing.z * p_state->settings.probe_count.z);
	push_constant.debug_bounds_min[0] = debug_bounds_min.x;
	push_constant.debug_bounds_min[1] = debug_bounds_min.y;
	push_constant.debug_bounds_min[2] = debug_bounds_min.z;
	push_constant.debug_bounds_inv_size[0] = 1.0f / debug_bounds_size.x;
	push_constant.debug_bounds_inv_size[1] = 1.0f / debug_bounds_size.y;
	push_constant.debug_bounds_inv_size[2] = 1.0f / debug_bounds_size.z;
	static_assert(sizeof(GBufferPushConstant) == 64);

	const Size2i screen_size = render_buffers->get_internal_size();
	RD::get_singleton()->draw_command_begin_label("DDGI GBuffer");
	RD::RaytracingListID raytracing_list = RD::get_singleton()->raytracing_list_begin();
	RD::get_singleton()->raytracing_list_bind_raytracing_pipeline(raytracing_list, gbuffer_pipeline);
	RD::get_singleton()->raytracing_list_bind_uniform_set(raytracing_list, uniform_set, 0);
	RD::get_singleton()->raytracing_list_bind_uniform_set(raytracing_list, bindless_set, 1);
	p_rt_service.register_raytracing_buffer_dependencies(raytracing_list, p_snapshot);
	RD::get_singleton()->raytracing_list_set_push_constant(raytracing_list, &push_constant, sizeof(GBufferPushConstant));
	RD::get_singleton()->raytracing_list_trace_rays(raytracing_list, 0, gbuffer_hit_sbt, screen_size.x, screen_size.y, 1);
	RD::get_singleton()->raytracing_list_end();
	RD::get_singleton()->draw_command_end_label();

	return true;
}

bool DDGI::update_probes(
		const Ref<DDGIState> &p_state,
		const RendererSceneRenderImplementation::RTSceneSnapshot &p_snapshot,
		RendererSceneRenderImplementation::RenderRaytracing &p_rt_service,
		const RenderDataRD *p_render_data,
		RID p_sky_radiance) {
	ERR_FAIL_COND_V(p_state.is_null(), false);
	ERR_FAIL_NULL_V(p_render_data, false);
	ERR_FAIL_COND_V(p_render_data->render_buffers.is_null(), false);
	ERR_FAIL_COND_V(!p_state->is_configured_for(p_render_data->render_buffers.ptr()), false);
	ERR_FAIL_COND_V(!p_state->current_grid.valid || p_state->prepared_scene_pass == 0, false);
	ERR_FAIL_COND_V(!p_snapshot.is_valid(), false);
	ERR_FAIL_COND_V(!p_snapshot.consumers.has_flag(RT_SCENE_CONSUMER_DDGI), false);
	ERR_FAIL_COND_V(
			!probe_update_pipeline.is_valid() ||
					!probe_update_hit_sbt.is_valid() ||
					!probe_blend_pipeline.is_valid(),
			false);

	if (!p_state->resources_ready) {
		return false;
	}

	DDGIGridData grid_data;
	grid_data.origin[0] = p_state->current_grid.origin.x;
	grid_data.origin[1] = p_state->current_grid.origin.y;
	grid_data.origin[2] = p_state->current_grid.origin.z;
	grid_data.probe_count[0] = p_state->current_grid.probe_count.x;
	grid_data.probe_count[1] = p_state->current_grid.probe_count.y;
	grid_data.probe_count[2] = p_state->current_grid.probe_count.z;
	grid_data.probe_spacing[0] = p_state->current_grid.probe_spacing.x;
	grid_data.probe_spacing[1] = p_state->current_grid.probe_spacing.y;
	grid_data.probe_spacing[2] = p_state->current_grid.probe_spacing.z;
	grid_data.scroll_delta[0] = p_state->current_grid.scroll_delta.x;
	grid_data.scroll_delta[1] = p_state->current_grid.scroll_delta.y;
	grid_data.scroll_delta[2] = p_state->current_grid.scroll_delta.z;
	grid_data.max_ray_distance = p_state->settings.max_ray_distance;
	grid_data.hysteresis = p_state->settings.hysteresis;
	grid_data.normal_bias = p_state->settings.normal_bias;
	grid_data.view_bias = p_state->settings.view_bias;
	grid_data.energy = p_state->settings.energy;
	grid_data.rays_per_probe = p_state->settings.rays_per_probe;
	grid_data.frame_index = uint32_t(p_state->prepared_scene_pass);
	// grid_data.frame_index = 0; //Debug 排除时阈

	grid_data.current_atlas_index = p_state->current_atlas_index;
	grid_data.history_reset = p_state->history_reset ? 1 : 0;
	if (RD::get_singleton()->buffer_update(p_state->grid_uniform_buffer, 0, sizeof(DDGIGridData), &grid_data) != OK) {
		return false;
	}

	RendererSceneRenderImplementation::RTViewportState *rt_state =
			p_rt_service.get_viewport_state(p_snapshot);
	ERR_FAIL_NULL_V(rt_state, false);

	RendererSceneRenderImplementation::RT_LightData light_data[RendererSceneRenderImplementation::RT_LIGHTS_MAX] = {};
	const uint32_t light_count = p_rt_service.gather_lights(
			p_render_data,
			light_data,
			RendererSceneRenderImplementation::RT_LIGHTS_MAX);
	const uint32_t light_buffer_size =
			RendererSceneRenderImplementation::RT_LIGHTS_MAX *
			sizeof(RendererSceneRenderImplementation::RT_LightData);
	if (!rt_state->light_buffer.is_valid()) {
		rt_state->light_buffer = RD::get_singleton()->storage_buffer_create(light_buffer_size);
		if (rt_state->light_buffer.is_valid()) {
			RD::get_singleton()->set_resource_name(rt_state->light_buffer, "DDGI Light Buffer");
		}
	}
	if (!rt_state->light_buffer.is_valid() ||
			RD::get_singleton()->buffer_update(rt_state->light_buffer, 0, light_buffer_size, light_data) != OK) {
		return false;
	}

	TextureStorage *texture_storage = TextureStorage::get_singleton();
	ERR_FAIL_NULL_V(texture_storage, false);
	MaterialStorage *material_storage = MaterialStorage::get_singleton();
	ERR_FAIL_NULL_V(material_storage, false);
	MeshStorage *mesh_storage = MeshStorage::get_singleton();
	ERR_FAIL_NULL_V(mesh_storage, false);

	RID sky_radiance = p_state->settings.read_sky ? p_sky_radiance : RID();
	if (!sky_radiance.is_valid()) {
		sky_radiance = texture_storage->texture_rd_get_default(
				use_radiance_octmap_array
						? TextureStorage::DEFAULT_RD_TEXTURE_2D_ARRAY_BLACK
						: TextureStorage::DEFAULT_RD_TEXTURE_BLACK);
	}

	const uint32_t history_atlas_index = p_state->current_atlas_index;
	const uint32_t output_atlas_index = 1u - history_atlas_index;
	ERR_FAIL_COND_V(
			!p_state->irradiance_atlas[history_atlas_index].is_valid() ||
					!p_state->distance_atlas[history_atlas_index].is_valid() ||
					!p_state->irradiance_atlas[output_atlas_index].is_valid() ||
					!p_state->distance_atlas[output_atlas_index].is_valid(),
			false);

	const RID scene_uniform_buffer = p_render_data->scene_data->get_uniform_buffer();
	ERR_FAIL_COND_V(!scene_uniform_buffer.is_valid(), false);
	ERR_FAIL_COND_V(
			p_snapshot.instance_count > 0 &&
					(!p_snapshot.geometry_buffer.is_valid() || !p_snapshot.material_buffer.is_valid()),
			false);
	const RID default_storage_buffer = mesh_storage->get_default_rd_storage_buffer();
	const RID radiance_sampler = material_storage->sampler_rd_get_default(
			RSE::CANVAS_ITEM_TEXTURE_FILTER_LINEAR_WITH_MIPMAPS,
			RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
	const RID ddgi_linear_sampler = material_storage->sampler_rd_get_default(
			RSE::CANVAS_ITEM_TEXTURE_FILTER_LINEAR,
			RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);

	const RID probe_update_shader_rid =
			probe_update_shader.version_get_shader(probe_update_shader_version, 0);
	ERR_FAIL_COND_V(!probe_update_shader_rid.is_valid(), false);

	LocalVector<RD::Uniform> trace_uniforms;
	trace_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 0, p_state->grid_uniform_buffer));
	trace_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_ACCELERATION_STRUCTURE, 1, p_snapshot.tlas));
	trace_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 2, scene_uniform_buffer));
	trace_uniforms.push_back(RD::Uniform(
			RD::UNIFORM_TYPE_STORAGE_BUFFER,
			3,
			p_snapshot.geometry_buffer.is_valid() ? p_snapshot.geometry_buffer : default_storage_buffer));
	trace_uniforms.push_back(RD::Uniform(
			RD::UNIFORM_TYPE_STORAGE_BUFFER,
			5,
			p_snapshot.material_buffer.is_valid() ? p_snapshot.material_buffer : default_storage_buffer));
	trace_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 7, sky_radiance));
	trace_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER, 8, radiance_sampler));
	trace_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 13, rt_state->light_buffer));
	trace_uniforms.push_back(RD::Uniform(
			RD::UNIFORM_TYPE_SAMPLER,
			24,
			material_storage->sampler_rd_get_default(
					RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST_WITH_MIPMAPS,
					RSE::CANVAS_ITEM_TEXTURE_REPEAT_ENABLED)));
	trace_uniforms.push_back(RD::Uniform(
			RD::UNIFORM_TYPE_SAMPLER,
			25,
			material_storage->sampler_rd_get_default(
					RSE::CANVAS_ITEM_TEXTURE_FILTER_LINEAR_WITH_MIPMAPS,
					RSE::CANVAS_ITEM_TEXTURE_REPEAT_ENABLED)));
	trace_uniforms.push_back(RD::Uniform(
			RD::UNIFORM_TYPE_TEXTURE,
			26,
			p_state->irradiance_atlas[history_atlas_index]));
	trace_uniforms.push_back(RD::Uniform(
			RD::UNIFORM_TYPE_TEXTURE,
			27,
			p_state->distance_atlas[history_atlas_index]));
	trace_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER, 28, ddgi_linear_sampler));
	trace_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 29, p_state->ray_data));

	UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();
	ERR_FAIL_NULL_V(uniform_set_cache, false);
	const RID trace_uniform_set =
			uniform_set_cache->get_cache_vec(probe_update_shader_rid, 0, trace_uniforms);
	ERR_FAIL_COND_V(
			!trace_uniform_set.is_valid() ||
					!RD::get_singleton()->uniform_set_is_valid(trace_uniform_set),
			false);
	const RID bindless_set =
			p_rt_service.finalize_bindless_uniform_set(probe_update_shader_rid, 1);
	ERR_FAIL_COND_V(
			!bindless_set.is_valid() ||
					!RD::get_singleton()->uniform_set_is_valid(bindless_set),
			false);

	const RID probe_blend_shader_rid =
			probe_blend_shader.version_get_shader(probe_blend_shader_version, 0);
	ERR_FAIL_COND_V(!probe_blend_shader_rid.is_valid(), false);
	LocalVector<RD::Uniform> blend_uniforms;
	blend_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 0, p_state->grid_uniform_buffer));
	blend_uniforms.push_back(RD::Uniform(
			RD::UNIFORM_TYPE_TEXTURE,
			26,
			p_state->irradiance_atlas[history_atlas_index]));
	blend_uniforms.push_back(RD::Uniform(
			RD::UNIFORM_TYPE_TEXTURE,
			27,
			p_state->distance_atlas[history_atlas_index]));
	blend_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER, 28, ddgi_linear_sampler));
	blend_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 29, p_state->ray_data));
	blend_uniforms.push_back(RD::Uniform(
			RD::UNIFORM_TYPE_IMAGE,
			30,
			p_state->irradiance_atlas[output_atlas_index]));
	blend_uniforms.push_back(RD::Uniform(
			RD::UNIFORM_TYPE_IMAGE,
			31,
			p_state->distance_atlas[output_atlas_index]));
	const RID blend_uniform_set =
			uniform_set_cache->get_cache_vec(probe_blend_shader_rid, 0, blend_uniforms);
	ERR_FAIL_COND_V(
			!blend_uniform_set.is_valid() ||
					!RD::get_singleton()->uniform_set_is_valid(blend_uniform_set),
			false);

	ProbeUpdatePushConstant push_constant = {};
	push_constant.light_count = light_count;
	static_assert(sizeof(ProbeUpdatePushConstant) == 16);

	RD::get_singleton()->draw_command_begin_label("DDGI Probe Update");
	RD::RaytracingListID raytracing_list = RD::get_singleton()->raytracing_list_begin();
	RD::get_singleton()->raytracing_list_bind_raytracing_pipeline(raytracing_list, probe_update_pipeline);
	RD::get_singleton()->raytracing_list_bind_uniform_set(raytracing_list, trace_uniform_set, 0);
	RD::get_singleton()->raytracing_list_bind_uniform_set(raytracing_list, bindless_set, 1);
	p_rt_service.register_raytracing_buffer_dependencies(raytracing_list, p_snapshot);
	RD::get_singleton()->raytracing_list_set_push_constant(
			raytracing_list,
			&push_constant,
			sizeof(ProbeUpdatePushConstant));
	RD::get_singleton()->raytracing_list_trace_rays(
			raytracing_list,
			0,
			probe_update_hit_sbt,
			p_state->settings.rays_per_probe,
			uint32_t(p_state->settings.probe_count.x * p_state->settings.probe_count.y),
			uint32_t(p_state->settings.probe_count.z));
	RD::get_singleton()->raytracing_list_end();

	const uint32_t distance_width =
			uint32_t(p_state->settings.probe_count.x) *
			(DDGI_DISTANCE_TEXELS + DDGI_ATLAS_BORDER * 2);
	const uint32_t distance_height =
			uint32_t(p_state->settings.probe_count.y) *
			(DDGI_DISTANCE_TEXELS + DDGI_ATLAS_BORDER * 2);
	RD::ComputeListID compute_list = RD::get_singleton()->compute_list_begin();
	RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, probe_blend_pipeline);
	RD::get_singleton()->compute_list_bind_uniform_set(compute_list, blend_uniform_set, 0);
	RD::get_singleton()->compute_list_dispatch_threads(
			compute_list,
			distance_width,
			distance_height,
			uint32_t(p_state->settings.probe_count.z));
	RD::get_singleton()->compute_list_end();
	RD::get_singleton()->draw_command_end_label();

	return true;
}

bool DDGI::resolve(
		const Ref<DDGIState> &p_state,
		const Ref<RenderSceneBuffersRD> &p_render_buffers,
		const RenderDataRD *p_render_data,
		RID p_depth,
		RID p_normal_roughness) {
	ERR_FAIL_COND_V(p_state.is_null(), false);
	ERR_FAIL_COND_V(p_render_buffers.is_null(), false);
	ERR_FAIL_NULL_V(p_render_data, false);
	ERR_FAIL_COND_V(!p_state->resources_ready, false);
	ERR_FAIL_COND_V(!p_depth.is_valid() || !p_normal_roughness.is_valid(), false);

	if (!ensure_gi_outputs(p_render_buffers)) {
		return false;
	}

	UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();
	ERR_FAIL_NULL_V(uniform_set_cache, false);
	MaterialStorage *material_storage = MaterialStorage::get_singleton();
	ERR_FAIL_NULL_V(material_storage, false);

	const Size2i screen_size = p_render_buffers->get_internal_size();
	ResolvePushConstant push_constant = {};
	push_constant.screen_size[0] = screen_size.x;
	push_constant.screen_size[1] = screen_size.y;
	push_constant.energy = p_state->settings.energy;

	const RID nearest_sampler = material_storage->sampler_rd_get_default(
			RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST,
			RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
	const RID linear_sampler = material_storage->sampler_rd_get_default(
			RSE::CANVAS_ITEM_TEXTURE_FILTER_LINEAR,
			RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
	const RID shader = resolve_shader.version_get_shader(resolve_shader_version, 0);
	ERR_FAIL_COND_V(!shader.is_valid() || !resolve_pipeline.is_valid(), false);

	const RID scene_uniform_buffer = p_render_data->scene_data->get_uniform_buffer();
	ERR_FAIL_COND_V(!scene_uniform_buffer.is_valid(), false);
	const uint32_t output_atlas_index = 1u - p_state->current_atlas_index;

	LocalVector<RD::Uniform> uniforms;
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 0, p_state->grid_uniform_buffer));
	uniforms.push_back(RD::Uniform(
			RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE,
			1,
			Vector<RID>({ nearest_sampler, p_depth })));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 2, scene_uniform_buffer));
	uniforms.push_back(RD::Uniform(
			RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE,
			3,
			Vector<RID>({ nearest_sampler, p_normal_roughness })));
	uniforms.push_back(RD::Uniform(
			RD::UNIFORM_TYPE_IMAGE,
			4,
			p_render_buffers->get_texture(RB_SCOPE_GI, RB_TEX_AMBIENT)));
	uniforms.push_back(RD::Uniform(
			RD::UNIFORM_TYPE_IMAGE,
			5,
			p_render_buffers->get_texture(RB_SCOPE_GI, RB_TEX_REFLECTION)));
	uniforms.push_back(RD::Uniform(
			RD::UNIFORM_TYPE_TEXTURE,
			26,
			p_state->irradiance_atlas[output_atlas_index]));
	uniforms.push_back(RD::Uniform(
			RD::UNIFORM_TYPE_TEXTURE,
			27,
			p_state->distance_atlas[output_atlas_index]));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER, 28, linear_sampler));
	const RID uniform_set = uniform_set_cache->get_cache_vec(shader, 0, uniforms);
	if (!uniform_set.is_valid() || !RD::get_singleton()->uniform_set_is_valid(uniform_set)) {
		return false;
	}

	RD::get_singleton()->draw_command_begin_label("DDGI Resolve");
	RD::ComputeListID compute_list = RD::get_singleton()->compute_list_begin();
	RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, resolve_pipeline);
	RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set, 0);
	RD::get_singleton()->compute_list_set_push_constant(compute_list, &push_constant, sizeof(ResolvePushConstant));
	RD::get_singleton()->compute_list_dispatch_threads(compute_list, screen_size.x, screen_size.y, 1);
	RD::get_singleton()->compute_list_end();
	RD::get_singleton()->draw_command_end_label();
	return true;
}

} // namespace RendererRD
