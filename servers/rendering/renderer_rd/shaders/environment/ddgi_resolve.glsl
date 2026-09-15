#[compute]

#version 450

#VERSION_DEFINES

#define MAX_VIEWS 2

#include "../oct_inc.glsl"
#include "../scene_data_inc.glsl"
#include "ddgi_common_inc.glsl"

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 1) uniform sampler2D source_depth;

layout(set = 0, binding = 2, std140) uniform SceneDataBlock {
	SceneData data;
	SceneData prev_data;
}
scene_data_block;

layout(set = 0, binding = 3) uniform sampler2D source_normal_roughness;
layout(rgba16f, set = 0, binding = 4) uniform restrict writeonly image2D ambient_output;
layout(rgba16f, set = 0, binding = 5) uniform restrict writeonly image2D reflection_output;

layout(set = 0, binding = 26) uniform texture2DArray irradiance_atlas;
layout(set = 0, binding = 27) uniform texture2DArray distance_atlas;
layout(set = 0, binding = 28) uniform sampler ddgi_linear_sampler;

layout(push_constant, std430) uniform Params {
	ivec2 screen_size;
	float energy;
	float pad;
}
params;

mat4 get_inverse_view_matrix() {
	return transpose(mat4(
			scene_data_block.data.inv_view_matrix[0],
			scene_data_block.data.inv_view_matrix[1],
			scene_data_block.data.inv_view_matrix[2],
			vec4(0.0, 0.0, 0.0, 1.0)));
}

vec3 reconstruct_world_position(ivec2 p_pixel, float p_depth, mat4 p_inverse_view) {
	vec2 uv = (vec2(p_pixel) + vec2(0.5)) / vec2(params.screen_size);
	vec2 ndc = uv * 2.0 - 1.0;
	vec4 view_position =
			scene_data_block.data.inv_projection_matrix * vec4(ndc, p_depth, 1.0);
	view_position.xyz /= view_position.w;
	return (p_inverse_view * vec4(view_position.xyz, 1.0)).xyz;
}

void main() {
	ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(pixel, params.screen_size))) {
		return;
	}

	float depth = texelFetch(source_depth, pixel, 0).r;
	vec4 ambient = vec4(0.0);

	vec3 encoded_normal =
			texelFetch(source_normal_roughness, pixel, 0).xyz * 2.0 - 1.0;
	float normal_length_squared = dot(encoded_normal, encoded_normal);
	if (depth > 0.0 && normal_length_squared > 0.25) {
		mat4 inverse_view = get_inverse_view_matrix();
		vec3 world_position =
				reconstruct_world_position(pixel, depth, inverse_view);
		vec3 view_normal = encoded_normal * inversesqrt(normal_length_squared);
		vec3 world_normal = normalize(mat3(inverse_view) * view_normal);
		vec3 camera_position = inverse_view[3].xyz;
		vec3 ray_direction =
				ddgi_safe_normalize(world_position - camera_position, -world_normal);


		float total_weight;
		vec3 irradiance = ddgi_sample_irradiance(
				irradiance_atlas,
				distance_atlas,
				ddgi_linear_sampler,
				ddgi_grid.origin.xyz,
				world_position,
				world_normal,
				world_normal,
				ray_direction,
				total_weight);
		if (total_weight > 1e-6) {
			// Forward+ multiplies this ambient term by the material albedo.
			ambient = vec4(
					max(irradiance * (max(params.energy, 0.0) / PI), vec3(0.0)),
					1.0);
		}
	}

	imageStore(ambient_output, pixel, ambient);
	imageStore(reflection_output, pixel, vec4(0.0));
}
