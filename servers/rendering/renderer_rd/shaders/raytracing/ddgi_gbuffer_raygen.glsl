#[raygen]

#version 460

#VERSION_DEFINES

#pragma shader_stage(raygen)
#extension GL_EXT_ray_tracing : enable //启动 RayTracing
#extension GL_EXT_ray_query : require //启动光线查询
#extension GL_EXT_buffer_reference : require //启动缓冲区引用 BDA
#extension GL_EXT_buffer_reference2 : require //启动缓冲区引用2
#extension GL_ARB_gpu_shader_int64 : require //启动GPU着色器整数64位
#extension GL_EXT_nonuniform_qualifier : require //启动非均匀限定符 descriptor index

#define GLSL 1
#define RT_STAGE_RAYGEN 1
#define USE_RAY_QUERY_SHADOWS 1

// Full-resolution camera G-buffer pass modeled after the RTXGI test harness.
// Inline ray queries keep it independent from the path-tracing payload/SBT.
// This first version evaluates cached triangle material data; custom procedural
// intersection code and full ShaderMaterial evaluation require a dedicated hit pipeline.

// clang-format off
#include "raytracing_inc.glsl"
#include "../scene_data_inc.glsl"
#include "brdf_inc.glsl"
#include "raytracing_hit_inc.glsl"
// clang-format on

layout(set = 0, binding = 0, rgba8) uniform restrict writeonly image2D gbuffer_albedo_flags;
layout(set = 0, binding = 29, rgba32f) uniform restrict writeonly image2D gbuffer_position_hit_t;
layout(set = 0, binding = 30, rgba32f) uniform restrict writeonly image2D gbuffer_normal;
layout(set = 0, binding = 31, rgba32f) uniform restrict writeonly image2D gbuffer_direct_diffuse;
layout(set = 0, binding = 32, rgba8) uniform restrict writeonly image2D gbuffer_debug;

layout(set = 0, binding = 1) uniform accelerationStructureEXT tlas;

layout(set = 0, binding = 2, std140) uniform SceneDataBlock {
	SceneData data;
	SceneData prev_data;
}
scene_data_block;

layout(set = 0, binding = 3, std430) readonly buffer GeometryBuffer {
	GeometryData geometries[];
};

layout(set = 0, binding = 5, std430) readonly buffer MaterialBuffer {
	MaterialData materials[];
};

#ifdef USE_RADIANCE_OCTMAP_ARRAY
layout(set = 0, binding = 7) uniform texture2DArray radiance_octmap;
#else
layout(set = 0, binding = 7) uniform texture2D radiance_octmap;
#endif
layout(set = 0, binding = 8) uniform sampler radiance_sampler;

layout(set = 1, binding = 0) uniform texture2D bindless_textures[];
layout(set = 0, binding = 24) uniform sampler SAMPLER_NEAREST_WITH_MIPMAPS_REPEAT;
layout(set = 0, binding = 25) uniform sampler SAMPLER_LINEAR_WITH_MIPMAPS_REPEAT;

#define RT_LIGHT_BUFFER_BINDING 13
#include "raytracing_lights_inc.glsl"

layout(push_constant, std430) uniform Params {
	uint light_count;
	uint frame_index;
	float normal_bias;
	float view_bias;
	uint debug_mode;
	float pad0;
	float pad1;
	float pad2;
	vec4 debug_bounds_min;
	vec4 debug_bounds_inv_size;
}
params;

const float DDGI_GBUFFER_BACKGROUND = 0.0;
const float DDGI_GBUFFER_SURFACE = 1.0;
const uint DDGI_DEBUG_DISABLED = 0u;
const uint DDGI_DEBUG_ALBEDO = 1u;
const uint DDGI_DEBUG_WORLD_POSITION = 2u;
const uint DDGI_DEBUG_NORMAL = 3u;
const uint DDGI_DEBUG_DIRECT_DIFFUSE = 4u;

vec3 linear_to_srgb(vec3 p_color) {
	p_color = max(p_color, vec3(0.0));
	const vec3 a = vec3(0.055);
	return mix(
			(1.0 + a) * pow(p_color, vec3(1.0 / 2.4)) - a,
			12.92 * p_color,
			lessThanEqual(p_color, vec3(0.0031308)));
}

void write_debug(
		ivec2 p_pixel,
		vec3 p_albedo_srgb,
		vec3 p_world_position,
		vec3 p_normal,
		vec3 p_direct_diffuse,
		bool p_surface_hit) {
	if (params.debug_mode == DDGI_DEBUG_DISABLED) {
		return;
	}

	vec3 debug_color = vec3(0.0);
	if (params.debug_mode == DDGI_DEBUG_ALBEDO) {
		debug_color = p_albedo_srgb;
	} else if (p_surface_hit) {
		if (params.debug_mode == DDGI_DEBUG_WORLD_POSITION) {
			debug_color = clamp(
					(p_world_position - params.debug_bounds_min.xyz) * params.debug_bounds_inv_size.xyz,
					vec3(0.0),
					vec3(1.0));
		} else if (params.debug_mode == DDGI_DEBUG_NORMAL) {
			debug_color = normalize(p_normal) * 0.5 + 0.5;
		} else if (params.debug_mode == DDGI_DEBUG_DIRECT_DIFFUSE) {
			vec3 mapped_diffuse = max(p_direct_diffuse, vec3(0.0));
			debug_color = linear_to_srgb(mapped_diffuse / (vec3(1.0) + mapped_diffuse));
		}
	}

	imageStore(gbuffer_debug, p_pixel, vec4(debug_color, 1.0));
}

vec4 sample_material_texture(uint p_texture_index, vec2 p_uv, uint p_material_flags) {
	if ((p_material_flags & 4u) != 0u) {
		return texture(sampler2D(bindless_textures[nonuniformEXT(p_texture_index)], SAMPLER_NEAREST_WITH_MIPMAPS_REPEAT), p_uv);
	}
	return texture(sampler2D(bindless_textures[nonuniformEXT(p_texture_index)], SAMPLER_LINEAR_WITH_MIPMAPS_REPEAT), p_uv);
}

vec3 sample_sky(vec3 p_world_direction) {
	mat3 camera_basis = mat3(scene_data_block.data.inv_view_matrix);
	mat3 world_to_sky = scene_data_block.data.radiance_inverse_xform * camera_basis;
	vec2 border = vec2(
			scene_data_block.data.radiance_border_size,
			1.0 - scene_data_block.data.radiance_border_size * 2.0);
	vec2 sky_uv = vec3_to_oct_with_border(world_to_sky * p_world_direction, border);

#ifdef USE_RADIANCE_OCTMAP_ARRAY
	vec3 sky = textureLod(sampler2DArray(radiance_octmap, radiance_sampler), vec3(sky_uv, 0.0), 0.0).rgb;
#else
	vec3 sky = textureLod(sampler2D(radiance_octmap, radiance_sampler), sky_uv, 0.0).rgb;
#endif
	return sky * scene_data_block.data.IBL_exposure_normalization;
}

void write_background(ivec2 p_pixel, vec3 p_ray_direction) {
	vec3 sky_srgb = linear_to_srgb(sample_sky(p_ray_direction));
	imageStore(gbuffer_albedo_flags, p_pixel, vec4(sky_srgb, DDGI_GBUFFER_BACKGROUND));
	imageStore(gbuffer_position_hit_t, p_pixel, vec4(0.0, 0.0, 0.0, -1.0));
	imageStore(gbuffer_normal, p_pixel, vec4(0.0));
	imageStore(gbuffer_direct_diffuse, p_pixel, vec4(0.0));
	write_debug(p_pixel, sky_srgb, vec3(0.0), vec3(0.0), vec3(0.0), false);
}

void main() {
	ivec2 pixel = ivec2(gl_LaunchIDEXT.xy);
	vec2 uv = (vec2(pixel) + vec2(0.5)) / vec2(gl_LaunchSizeEXT.xy);
	vec2 ndc = uv * 2.0 - 1.0;

	mat4 inv_view = transpose(mat4(
			scene_data_block.data.inv_view_matrix[0],
			scene_data_block.data.inv_view_matrix[1],
			scene_data_block.data.inv_view_matrix[2],
			vec4(0.0, 0.0, 0.0, 1.0)));
	vec4 view_near = scene_data_block.data.inv_projection_matrix * vec4(ndc, 1.0, 1.0);
	vec4 view_far = scene_data_block.data.inv_projection_matrix * vec4(ndc, 0.0, 1.0);
	view_near.xyz /= view_near.w;
	view_far.xyz /= view_far.w;

	vec3 ray_origin = (inv_view * vec4(view_near.xyz, 1.0)).xyz;
	vec3 ray_end = (inv_view * vec4(view_far.xyz, 1.0)).xyz;
	vec3 ray_segment = ray_end - ray_origin;
	float ray_length = length(ray_segment);
	vec3 ray_direction = ray_segment / max(ray_length, 0.0001);

//false
//candidate（当前候选）
//只在 rayQueryProceedEXT 还在跑、刚给出一个候选时
//true
//committed（已提交）
//循环结束后，查最终确认的那一次命中
	rayQueryEXT primary_query;
	rayQueryInitializeEXT(
			primary_query,
			tlas,
			gl_RayFlagsCullBackFacingTrianglesEXT,
			0x01,
			ray_origin,
			0.0001,
			ray_direction,
			max(ray_length, 0.0002));

	while (rayQueryProceedEXT(primary_query)) {
		if (rayQueryGetIntersectionTypeEXT(primary_query, false) == gl_RayQueryCandidateIntersectionTriangleEXT &&
				ray_query_alpha_test(
						rayQueryGetIntersectionInstanceCustomIndexEXT(primary_query, false),
						rayQueryGetIntersectionPrimitiveIndexEXT(primary_query, false),
						rayQueryGetIntersectionBarycentricsEXT(primary_query, false))) { //Alpha >= 0.5 则通过 类似在做AHS
			rayQueryConfirmIntersectionEXT(primary_query); //确认命中
		}
	}

	if (rayQueryGetIntersectionTypeEXT(primary_query, true) == gl_RayQueryCommittedIntersectionNoneEXT) {
		write_background(pixel, ray_direction);
		return;
	}

	uint geometry_index = rayQueryGetIntersectionInstanceCustomIndexEXT(primary_query, true);
	uint primitive_index = rayQueryGetIntersectionPrimitiveIndexEXT(primary_query, true);
	vec2 hit_barycentrics = rayQueryGetIntersectionBarycentricsEXT(primary_query, true);
	vec3 barycentrics = vec3(1.0 - hit_barycentrics.x - hit_barycentrics.y, hit_barycentrics);

	GeometryData geometry = geometries[geometry_index];
	uint i0;
	uint i1;
	uint i2;
	get_triangle_indices_ex(geometry, primitive_index, i0, i1, i2);

	vec2 material_uv = fetch_uv(geometry, i0, i1, i2, barycentrics);
	TBNResult object_tbn = fetch_tbn(geometry, i0, i1, i2, barycentrics);

	mat4x3 object_to_world = rayQueryGetIntersectionObjectToWorldEXT(primary_query, true);
	mat4x3 world_to_object = rayQueryGetIntersectionWorldToObjectEXT(primary_query, true);
	mat3 tangent_matrix = mat3(object_to_world[0].xyz, object_to_world[1].xyz, object_to_world[2].xyz);
	mat3 normal_matrix = transpose(mat3(world_to_object[0].xyz, world_to_object[1].xyz, world_to_object[2].xyz));

	vec3 geometry_normal = normalize(normal_matrix * object_tbn.normal);
	vec3 tangent = normalize(tangent_matrix * object_tbn.tangent);
	if (!rayQueryGetIntersectionFrontFaceEXT(primary_query, true)) {
		geometry_normal = -geometry_normal;
	}
	vec3 bitangent = normalize(cross(geometry_normal, tangent)) * object_tbn.bitangent_sign;

	MaterialData material = materials[geometry_index];
	material_uv = material_uv * material.uv1_scale + material.uv1_offset;

	vec4 albedo_sample = sample_material_texture(material.albedo_texture_idx, material_uv, material.flags);
	vec3 albedo = albedo_sample.rgb * material.albedo_color.rgb;
	vec3 orm = sample_material_texture(material.orm_texture_idx, material_uv, material.flags).rgb;
	float roughness = clamp(orm.g * material.roughness, 0.0, 1.0);
	float metalness = clamp(orm.b * material.metallic, 0.0, 1.0);

	vec3 shading_normal = geometry_normal;
	if ((material.flags & 1u) != 0u) {
		vec3 tangent_normal;
		tangent_normal.xy = sample_material_texture(material.normal_texture_idx, material_uv, material.flags).xy * 2.0 - 1.0;
		tangent_normal.z = sqrt(max(0.0, 1.0 - dot(tangent_normal.xy, tangent_normal.xy)));
		vec3 mapped_normal = normalize(
				tangent * tangent_normal.x +
				bitangent * tangent_normal.y +
				geometry_normal * tangent_normal.z);
		shading_normal = normalize(mix(geometry_normal, mapped_normal, material.normal_map_depth));
	}

	float hit_t = rayQueryGetIntersectionTEXT(primary_query, true);
	vec3 world_position = ray_origin + ray_direction * hit_t;
	vec3 biased_position = offset_ray_origin(
			world_position + geometry_normal * params.normal_bias - ray_direction * params.view_bias,
			geometry_normal);

	// Force the shared direct-light evaluator to return only the diffuse lobe:
	// zero F0 removes specular, while pre-applying (1 - metalness) preserves
	// the metallic workflow's diffuse reflectance.
	MaterialProperties diffuse_material;
	diffuse_material.baseColor = albedo * (1.0 - metalness);
	diffuse_material.metalness = 0.0;
	diffuse_material.emissive = vec3(0.0);
	diffuse_material.roughness = roughness;
	diffuse_material.dielectricF0 = 0.0;
	diffuse_material.transmissivness = 0.0;
	diffuse_material.opacity = 1.0;

	uint rng_state = init_rng(uvec2(pixel), params.frame_index, 0u);
	vec3 direct_diffuse = lights_evaluate_direct_lighting(
			biased_position,
			shading_normal,
			-ray_direction,
			diffuse_material,
			rng_state,
			false,
			params.light_count);

	vec3 albedo_srgb = linear_to_srgb(albedo);
	imageStore(gbuffer_albedo_flags, pixel, vec4(albedo_srgb, DDGI_GBUFFER_SURFACE));
	imageStore(gbuffer_position_hit_t, pixel, vec4(world_position, hit_t));
	imageStore(gbuffer_normal, pixel, vec4(shading_normal, 1.0));
	imageStore(gbuffer_direct_diffuse, pixel, vec4(direct_diffuse, 1.0));
	write_debug(pixel, albedo_srgb, world_position, shading_normal, direct_diffuse, true);
}