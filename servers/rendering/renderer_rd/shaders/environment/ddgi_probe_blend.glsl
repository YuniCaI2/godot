#[compute]

#version 450

#VERSION_DEFINES

#include "../oct_inc.glsl"
#include "ddgi_common_inc.glsl"

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 26) uniform texture2DArray previous_irradiance_atlas;
layout(set = 0, binding = 27) uniform texture2DArray previous_distance_atlas;
layout(set = 0, binding = 28) uniform sampler ddgi_linear_sampler;

layout(set = 0, binding = 29, std430) readonly buffer ProbeRayBuffer {
	vec4 probe_ray_data[];
};

layout(rgba16f, set = 0, binding = 30) uniform image2DArray irradiance_atlas;
layout(rg16f, set = 0, binding = 31) uniform image2DArray distance_atlas;

float history_weight(bool p_has_history, vec3 p_history) {
	if (!p_has_history || ddgi_grid.history_reset != 0u || dot(p_history, p_history) == 0.0) {
		return 0.0;
	}
	return clamp(ddgi_grid.hysteresis, 0.0, 1.0);
}

bool history_atlas_coord(ivec3 p_probe_coord, ivec2 p_local_coord, int p_tile_size, out ivec3 r_atlas_coord) {
	ivec3 history_probe = ddgi_history_probe_coord(p_probe_coord);
	if (!ddgi_probe_in_bounds(history_probe)) {
		r_atlas_coord = ivec3(0);
		return false;
	}
	r_atlas_coord = ddgi_probe_atlas_texel(history_probe, p_local_coord, p_tile_size);
	return true;
}

void update_irradiance_texel(ivec3 p_atlas_coord, ivec3 p_probe_coord, ivec2 p_local_coord) {
	const float tile_size = float(DDGI_IRRADIANCE_TEXELS + DDGI_ATLAS_BORDER * 2);
	vec2 tile_uv = (vec2(p_local_coord) + vec2(0.5)) / tile_size;
	vec3 texel_direction = oct_to_vec3_with_border(
			tile_uv,
			float(DDGI_IRRADIANCE_TEXELS) / tile_size); //忽略边缘在内部采样，边缘用来保持采样不溢出

	uint probe_index = ddgi_probe_index(p_probe_coord);
	uint ray_base = probe_index * ddgi_grid.rays_per_probe;
	vec4 frame_rotation = ddgi_frame_rotation();
	vec3 radiance_sum = vec3(0.0);
	float weight_sum = 0.0;

	for (uint ray_index = 0u; ray_index < ddgi_grid.rays_per_probe; ray_index++) {
		vec4 ray = probe_ray_data[ray_base + ray_index];
		if (ray.a < 0.0) {
			continue;
		}

		vec3 ray_direction = ddgi_probe_ray_direction(ray_index, frame_rotation);
		float weight = max(0.0, dot(texel_direction, ray_direction));
		radiance_sum += ray.rgb * weight;
		weight_sum += weight;
	}

	float epsilon = max(float(ddgi_grid.rays_per_probe) * 1e-9, 1e-6);
	vec3 irradiance = radiance_sum / (2.0 * max(weight_sum, epsilon));
	vec3 encoded = pow(max(irradiance, vec3(0.0)), vec3(1.0 / DDGI_IRRADIANCE_GAMMA));
	ivec3 previous_atlas_coord;
	bool has_history = history_atlas_coord(
			p_probe_coord,
			p_local_coord,
			DDGI_IRRADIANCE_TEXELS + DDGI_ATLAS_BORDER * 2,
			previous_atlas_coord);
	vec3 previous = has_history
			? texelFetch(
					sampler2DArray(previous_irradiance_atlas, ddgi_linear_sampler),
					previous_atlas_coord,
					0)
					  .rgb
			: vec3(0.0);
	float hysteresis = history_weight(has_history, previous);
	if (hysteresis > 0.0) {
		// 64 rays plus gamma-5 encoding turns a lucky sun hit into a bright
		// flash. Cap the new sample against history before blending.
		float previous_peak = max(previous.r, max(previous.g, previous.b));
		float encoded_peak = max(encoded.r, max(encoded.g, encoded.b));
		float peak_limit = max(previous_peak * 2.0, previous_peak + 0.08);
		if (encoded_peak > peak_limit && encoded_peak > 1e-4) {
			encoded *= peak_limit / encoded_peak;
		}
	}
	vec3 blended = mix(encoded, previous, hysteresis);
	imageStore(irradiance_atlas, p_atlas_coord, vec4(min(blended, vec3(65504.0)), 1.0));
}

void update_distance_texel(ivec3 p_atlas_coord, ivec3 p_probe_coord, ivec2 p_local_coord) {
	const float tile_size = float(DDGI_DISTANCE_TEXELS + DDGI_ATLAS_BORDER * 2);
	vec2 tile_uv = (vec2(p_local_coord) + vec2(0.5)) / tile_size;
	vec3 texel_direction = oct_to_vec3_with_border(
			tile_uv,
			float(DDGI_DISTANCE_TEXELS) / tile_size);

	uint probe_index = ddgi_probe_index(p_probe_coord);
	uint ray_base = probe_index * ddgi_grid.rays_per_probe;
	vec4 frame_rotation = ddgi_frame_rotation();
	float distance_cap =
			min(ddgi_grid.max_ray_distance, length(ddgi_grid.probe_spacing.xyz) * 1.5);
	vec2 moment_sum = vec2(0.0);
	float weight_sum = 0.0;

	for (uint ray_index = 0u; ray_index < ddgi_grid.rays_per_probe; ray_index++) {
		vec4 ray = probe_ray_data[ray_base + ray_index];
		vec3 ray_direction = ddgi_probe_ray_direction(ray_index, frame_rotation);
		float cosine = max(0.0, dot(texel_direction, ray_direction));
		float weight = pow(cosine, DDGI_DISTANCE_EXPONENT);
		float distance = min(abs(ray.a), distance_cap);
		moment_sum += vec2(distance, distance * distance) * weight;
		weight_sum += weight;
	}

	float epsilon = max(float(ddgi_grid.rays_per_probe) * 1e-9, 1e-6);
	vec2 moments;
	if (weight_sum > epsilon) {
		moments = moment_sum / (2.0 * weight_sum);
	} else {
		moments = vec2(distance_cap, distance_cap * distance_cap) * 0.5;
	}

	ivec3 previous_atlas_coord;
	bool has_history = history_atlas_coord(
			p_probe_coord,
			p_local_coord,
			DDGI_DISTANCE_TEXELS + DDGI_ATLAS_BORDER * 2,
			previous_atlas_coord);
	vec2 previous = has_history
			? texelFetch(
					sampler2DArray(previous_distance_atlas, ddgi_linear_sampler),
					previous_atlas_coord,
					0)
					  .rg
			: vec2(0.0);
	float hysteresis = history_weight(has_history, vec3(previous, 0.0));
	vec2 blended = mix(moments, previous, hysteresis);
	imageStore(distance_atlas, p_atlas_coord, vec4(blended, 0.0, 1.0));
}

void main() {
	ivec3 atlas_coord = ivec3(gl_GlobalInvocationID);
	if (atlas_coord.z >= ddgi_grid.probe_count.z) {
		return;
	}

	const int irradiance_tile_size =
			DDGI_IRRADIANCE_TEXELS + DDGI_ATLAS_BORDER * 2;
	ivec2 irradiance_size = ddgi_grid.probe_count.xy * irradiance_tile_size;
	if (all(lessThan(atlas_coord.xy, irradiance_size))) {
		ivec2 probe_xy = atlas_coord.xy / irradiance_tile_size;
		ivec2 local_coord = atlas_coord.xy % irradiance_tile_size;
		update_irradiance_texel(
				atlas_coord,
				ivec3(probe_xy, atlas_coord.z),
				local_coord);
	}

	const int distance_tile_size = DDGI_DISTANCE_TEXELS + DDGI_ATLAS_BORDER * 2;
	ivec2 distance_size = ddgi_grid.probe_count.xy * distance_tile_size;
	if (all(lessThan(atlas_coord.xy, distance_size))) {
		ivec2 probe_xy = atlas_coord.xy / distance_tile_size;
		ivec2 local_coord = atlas_coord.xy % distance_tile_size;
		update_distance_texel(
				atlas_coord,
				ivec3(probe_xy, atlas_coord.z),
				local_coord);
	}
}
