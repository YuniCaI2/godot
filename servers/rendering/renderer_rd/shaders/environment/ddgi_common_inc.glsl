#ifndef DDGI_COMMON_INC_GLSL
#define DDGI_COMMON_INC_GLSL

#ifndef PI
#define PI 3.14159265358979323846
#endif

#define DDGI_TWO_PI (2.0 * PI)
#define DDGI_IRRADIANCE_TEXELS 8
#define DDGI_DISTANCE_TEXELS 16
#define DDGI_ATLAS_BORDER 1
#define DDGI_IRRADIANCE_GAMMA 5.0
#define DDGI_DISTANCE_EXPONENT 50.0

// Must match DDGIGridData in ddgi.cpp.
layout(set = 0, binding = 0, std140) uniform DDGIGridBlock {
	vec4 origin;
	ivec4 probe_count;
	vec4 probe_spacing;
	ivec4 scroll_delta;
	float max_ray_distance;
	float hysteresis;
	float normal_bias;
	float view_bias;
	float energy;
	uint rays_per_probe;
	uint frame_index;
	uint current_atlas_index;
	uint history_reset;
}
ddgi_grid;

uint ddgi_total_probe_count() {
	return uint(ddgi_grid.probe_count.x * ddgi_grid.probe_count.y * ddgi_grid.probe_count.z);
}

vec3 ddgi_pre_grid_position() {
	return ddgi_grid.origin.xyz - vec3(ddgi_grid.scroll_delta.xyz) * ddgi_grid.probe_spacing.xyz;
}

bool ddgi_probe_in_bounds(ivec3 p_probe_coord) {
	return all(greaterThanEqual(p_probe_coord, ivec3(0))) &&
			all(lessThan(p_probe_coord, ddgi_grid.probe_count.xyz));
}

// Previous-frame probe that occupied this world cell. Out of bounds means the
// cell just scrolled in and has no atlas history.
ivec3 ddgi_history_probe_coord(ivec3 p_probe_coord) {
	return p_probe_coord + ddgi_grid.scroll_delta.xyz;
}

ivec3 ddgi_probe_atlas_texel(ivec3 p_probe_coord, ivec2 p_local_coord, int p_tile_size) {
	return ivec3(p_probe_coord.xy * p_tile_size + p_local_coord, p_probe_coord.z);
}

uint ddgi_probe_index(ivec3 p_probe_coord) {
	return uint(p_probe_coord.x +
			ddgi_grid.probe_count.x *
					(p_probe_coord.y + ddgi_grid.probe_count.y * p_probe_coord.z));
}

ivec3 ddgi_probe_coord(uint p_probe_index) {
	uint count_x = uint(ddgi_grid.probe_count.x);
	uint count_y = uint(ddgi_grid.probe_count.y);
	uint xy_count = count_x * count_y;
	return ivec3(
			int(p_probe_index % count_x),
			int((p_probe_index / count_x) % count_y),
			int(p_probe_index / xy_count));
}

vec3 ddgi_probe_position(ivec3 p_probe_coord) {
	return ddgi_grid.origin.xyz + vec3(p_probe_coord) * ddgi_grid.probe_spacing.xyz;
}

uint ddgi_hash(uint p_value) {
	p_value ^= p_value >> 16;
	p_value *= 0x7feb352du;
	p_value ^= p_value >> 15;
	p_value *= 0x846ca68bu;
	p_value ^= p_value >> 16;
	return p_value;
}

float ddgi_hash_float(uint p_value) {
	return float(ddgi_hash(p_value)) * (1.0 / 4294967296.0);
}

vec4 ddgi_frame_rotation() {
	float u1 = ddgi_hash_float(ddgi_grid.frame_index * 3u + 0u);
	float u2 = ddgi_hash_float(ddgi_grid.frame_index * 3u + 1u);
	float u3 = ddgi_hash_float(ddgi_grid.frame_index * 3u + 2u);
	float sqrt_one_minus_u1 = sqrt(max(0.0, 1.0 - u1));
	float sqrt_u1 = sqrt(max(0.0, u1));
	return vec4(
			sqrt_one_minus_u1 * sin(DDGI_TWO_PI * u2),
			sqrt_one_minus_u1 * cos(DDGI_TWO_PI * u2),
			sqrt_u1 * sin(DDGI_TWO_PI * u3),
			sqrt_u1 * cos(DDGI_TWO_PI * u3));
}

vec3 ddgi_rotate_by_quaternion(vec3 p_direction, vec4 p_quaternion) {
	return p_direction +
			2.0 * cross(p_quaternion.xyz, cross(p_quaternion.xyz, p_direction) + p_quaternion.w * p_direction);
}

vec3 ddgi_probe_ray_direction(uint p_ray_index, vec4 p_frame_rotation) {
	float ray_count = float(max(ddgi_grid.rays_per_probe, 1u));
	float z = 1.0 - (2.0 * (float(p_ray_index) + 0.5)) / ray_count;
	float radius = sqrt(max(0.0, 1.0 - z * z));
	float phi = DDGI_TWO_PI * fract(float(p_ray_index) * 0.6180339887498948);
	vec3 direction = vec3(radius * cos(phi), radius * sin(phi), z);
	return normalize(ddgi_rotate_by_quaternion(direction, p_frame_rotation));
}

vec3 ddgi_probe_ray_direction(uint p_ray_index) {
	return ddgi_probe_ray_direction(p_ray_index, ddgi_frame_rotation());
}

vec2 ddgi_probe_atlas_uv(ivec3 p_probe_coord, vec3 p_direction, int p_interior_texels) {
	int tile_size = p_interior_texels + DDGI_ATLAS_BORDER * 2;
	vec2 atlas_size = vec2(ddgi_grid.probe_count.xy * tile_size);
	vec2 oct_uv = vec3_to_oct(normalize(p_direction));
	vec2 texel = vec2(p_probe_coord.xy * tile_size + ivec2(DDGI_ATLAS_BORDER)) +
			oct_uv * float(p_interior_texels);
	return texel / atlas_size;
}

float ddgi_chebyshev_visibility(vec2 p_stored_moments, float p_distance) {
	// Probe blending stores half moments to match the RTXGI normalization.
	vec2 moments = p_stored_moments * 2.0;
	float mean = moments.x;
	if (p_distance <= mean) {
		return 1.0;
	}

	float variance = abs(moments.y - mean * mean); //E(x^2) - E(x)^2 得到方差
	float delta = p_distance - mean;
	float visibility = variance / max(variance + delta * delta, 1e-6);
	return visibility * visibility * visibility;
}

vec3 ddgi_safe_normalize(vec3 p_value, vec3 p_fallback) {
	float value_length_squared = dot(p_value, p_value);
	return value_length_squared > 1e-10 ? p_value * inversesqrt(value_length_squared) : p_fallback;
}

// Returns linear irradiance. The caller applies the diffuse 1 / PI term.
// p_grid_origin must match the atlas being sampled: current origin for this
// frame's atlas, ddgi_pre_grid_position() for the previous atlas after scroll.
vec3 ddgi_sample_irradiance(
		texture2DArray p_irradiance_atlas,
		texture2DArray p_distance_atlas,
		sampler p_linear_sampler,
		vec3 p_grid_origin,
		vec3 p_world_position,
		vec3 p_bias_normal,
		vec3 p_shading_normal,
		vec3 p_ray_direction,
		out float r_total_weight) {
	r_total_weight = 0.0;

	vec3 grid_position = (p_world_position - p_grid_origin) / ddgi_grid.probe_spacing.xyz;
	vec3 probe_max = vec3(ddgi_grid.probe_count.xyz - ivec3(1));
	if (any(lessThan(grid_position, vec3(-0.5))) ||
			any(greaterThan(grid_position, probe_max + vec3(0.5)))) {
		return vec3(0.0);
	}

	vec3 normal = ddgi_safe_normalize(p_bias_normal, vec3(0.0, 1.0, 0.0));
	vec3 shading_normal = ddgi_safe_normalize(p_shading_normal, normal);
	vec3 ray_direction = ddgi_safe_normalize(p_ray_direction, -normal);
	vec3 biased_position =
			p_world_position + normal * ddgi_grid.normal_bias - ray_direction * ddgi_grid.view_bias;
	vec3 biased_grid_position = (biased_position - p_grid_origin) / ddgi_grid.probe_spacing.xyz;

	ivec3 max_base = max(ddgi_grid.probe_count.xyz - ivec3(2), ivec3(0));
	ivec3 base_probe = clamp(ivec3(floor(biased_grid_position)), ivec3(0), max_base);
	vec3 alpha = clamp(biased_grid_position - vec3(base_probe), vec3(0.0), vec3(1.0));

	vec3 irradiance_sum = vec3(0.0);
	for (uint corner_index = 0u; corner_index < 8u; corner_index++) {
		ivec3 corner = ivec3(
				int(corner_index & 1u),
				int((corner_index >> 1u) & 1u),
				int((corner_index >> 2u) & 1u));
		corner = min(corner, ddgi_grid.probe_count.xyz - ivec3(1));
		ivec3 probe_coord = base_probe + corner;

		vec3 corner_f = vec3(corner);
		vec3 trilinear = mix(vec3(1.0) - alpha, alpha, corner_f);
		trilinear = max(trilinear, vec3(0.001));
		float weight = trilinear.x * trilinear.y * trilinear.z;

		vec3 probe_position = p_grid_origin + vec3(probe_coord) * ddgi_grid.probe_spacing.xyz;
		vec3 direction_to_probe = ddgi_safe_normalize(probe_position - p_world_position, normal);
		float backface_weight = (dot(direction_to_probe, normal) + 1.0) * 0.5; // -1, 1 To 0, 1
		backface_weight = backface_weight * backface_weight + 0.2; // x To x^2 + 0.2

		vec3 probe_to_point = biased_position - probe_position;
		float probe_distance = length(probe_to_point);
		vec3 probe_direction = ddgi_safe_normalize(probe_to_point, normal);
		vec2 distance_uv =
				ddgi_probe_atlas_uv(probe_coord, probe_direction, DDGI_DISTANCE_TEXELS);
		vec2 stored_moments = textureLod(
				sampler2DArray(p_distance_atlas, p_linear_sampler),
				vec3(distance_uv, float(probe_coord.z)),
				0.0)
									  .rg;
		float visibility = ddgi_chebyshev_visibility(stored_moments, probe_distance);

		float directional_weight = max(1e-6, backface_weight * max(0.05, visibility));
		if (directional_weight < 0.2) {
			directional_weight *= directional_weight * directional_weight / (0.2 * 0.2);
		}
		weight *= directional_weight;

		vec2 irradiance_uv =
				ddgi_probe_atlas_uv(probe_coord, shading_normal, DDGI_IRRADIANCE_TEXELS);
		vec3 encoded_irradiance = textureLod(
				sampler2DArray(p_irradiance_atlas, p_linear_sampler),
				vec3(irradiance_uv, float(probe_coord.z)),
				0.0)
										  .rgb;
		vec3 decoded_for_blending = pow(
				max(encoded_irradiance, vec3(0.0)),
				vec3(DDGI_IRRADIANCE_GAMMA * 0.5));

		irradiance_sum += decoded_for_blending * weight;
		r_total_weight += weight;
	}

	if (r_total_weight <= 1e-6) {
		return vec3(0.0);
	}

	vec3 blended = irradiance_sum / r_total_weight;
	return DDGI_TWO_PI * blended * blended;
}

#endif // DDGI_COMMON_INC_GLSL
