#[compute]

#version 450

#VERSION_DEFINES

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform sampler2D source_depth;
layout(set = 0, binding = 1) uniform sampler2D source_normal_roughness;
layout(rgba16f, set = 0, binding = 2) uniform restrict writeonly image2D ambient_output;
layout(rgba16f, set = 0, binding = 3) uniform restrict writeonly image2D reflection_output;

layout(push_constant, std430) uniform Params {
	ivec2 screen_size;
	float energy;
	float pad;
}
params;

void main() {
	ivec2 position = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(position, params.screen_size))) {
		return;
	}

	float depth = texelFetch(source_depth, position, 0).r;
	vec4 ambient = vec4(0.0);

	if (depth > 0.0) {
		// Diagnostic only: this low-energy cool tint proves that reverse-Z
		// depth, normal/roughness, GI output allocation, and Forward+ bindings
		// are connected. Replace this block with the real DDGI resolve.
		vec3 encoded_normal = texelFetch(source_normal_roughness, position, 0).xyz * 2.0 - 1.0;
		float normal_factor = 0.55 + 0.45 * abs(encoded_normal.y) / max(length(encoded_normal), 0.0001);
		vec3 debug_diffuse = vec3(0.012, 0.026, 0.055) * normal_factor * max(params.energy, 0.0);
		ambient = vec4(debug_diffuse, 1.0);
	}

	imageStore(ambient_output, position, ambient);
	imageStore(reflection_output, position, vec4(0.0));
}
