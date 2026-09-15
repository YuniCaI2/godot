#[raygen]

#version 460

#VERSION_DEFINES

#pragma shader_stage(raygen)
#extension GL_EXT_ray_tracing : enable
#extension GL_EXT_ray_query : require
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference2 : require
#extension GL_ARB_gpu_shader_int64 : require
#extension GL_EXT_nonuniform_qualifier : require

#define GLSL 1
#define RT_STAGE_RAYGEN 1
#define USE_RAY_QUERY_SHADOWS 1

// clang-format off
#include "../raytracing/raytracing_inc.glsl"
#include "../scene_data_inc.glsl"
#include "../raytracing/brdf_inc.glsl"
#include "../raytracing/raytracing_hit_inc.glsl"
#include "ddgi_common_inc.glsl"
// clang-format on

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

layout(set = 0, binding = 26) uniform texture2DArray previous_irradiance_atlas;
layout(set = 0, binding = 27) uniform texture2DArray previous_distance_atlas;
layout(set = 0, binding = 28) uniform sampler ddgi_linear_sampler;

layout(set = 0, binding = 29, std430) writeonly buffer ProbeRayBuffer {
	vec4 probe_ray_data[];
};

#define RT_LIGHT_BUFFER_BINDING 13
#include "../raytracing/raytracing_lights_inc.glsl"

layout(push_constant, std430) uniform Params {
	uint light_count;
	uint pad0;
	uint pad1;
	uint pad2;
}
params;

vec4 sample_material_texture(uint p_texture_index, vec2 p_uv, uint p_material_flags) {
	if ((p_material_flags & 4u) != 0u) {
		return textureLod(
				sampler2D(
						bindless_textures[nonuniformEXT(p_texture_index)],
						SAMPLER_NEAREST_WITH_MIPMAPS_REPEAT),
				p_uv,
				0.0);
	}
	return textureLod(
			sampler2D(
					bindless_textures[nonuniformEXT(p_texture_index)],
					SAMPLER_LINEAR_WITH_MIPMAPS_REPEAT),
			p_uv,
			0.0);
}

vec3 sample_sky(vec3 p_world_direction) {
	mat3 camera_basis = mat3(scene_data_block.data.inv_view_matrix);
	mat3 world_to_sky = scene_data_block.data.radiance_inverse_xform * camera_basis;
	vec2 border = vec2(
			scene_data_block.data.radiance_border_size,
			1.0 - scene_data_block.data.radiance_border_size * 2.0);
	vec2 sky_uv = vec3_to_oct_with_border(world_to_sky * p_world_direction, border);

#ifdef USE_RADIANCE_OCTMAP_ARRAY
	vec3 sky = textureLod(
			sampler2DArray(radiance_octmap, radiance_sampler),
			vec3(sky_uv, 0.0),
			0.0)
					   .rgb;
#else
	vec3 sky = textureLod(
			sampler2D(radiance_octmap, radiance_sampler),
			sky_uv,
			0.0)
					   .rgb;
#endif
	return sky * scene_data_block.data.IBL_exposure_normalization;
}

void store_probe_ray(uint p_probe_index, uint p_ray_index, vec3 p_radiance, float p_signed_distance) {
	uint output_index = p_probe_index * ddgi_grid.rays_per_probe + p_ray_index;
	probe_ray_data[output_index] = vec4(max(p_radiance, vec3(0.0)), p_signed_distance);
}

void main() {
	uint ray_index = gl_LaunchIDEXT.x;
	uint probe_xy = gl_LaunchIDEXT.y;
	uint probe_z = gl_LaunchIDEXT.z;
	uint probe_xy_count = uint(ddgi_grid.probe_count.x * ddgi_grid.probe_count.y);
	if (ray_index >= ddgi_grid.rays_per_probe ||
			probe_xy >= probe_xy_count ||
			probe_z >= uint(ddgi_grid.probe_count.z)) {
		return;
	}

	ivec3 probe_coord = ivec3(
			int(probe_xy % uint(ddgi_grid.probe_count.x)),
			int(probe_xy / uint(ddgi_grid.probe_count.x)),
			int(probe_z));
	uint probe_index = ddgi_probe_index(probe_coord);
	vec3 ray_origin = ddgi_probe_position(probe_coord);
	vec3 ray_direction = ddgi_probe_ray_direction(ray_index);

	rayQueryEXT query;
	rayQueryInitializeEXT(
			query,
			tlas,
			0u,
			0x01,
			ray_origin,
			0.001,
			ray_direction,
			ddgi_grid.max_ray_distance);

	while (rayQueryProceedEXT(query)) {
		if (rayQueryGetIntersectionTypeEXT(query, false) ==
						gl_RayQueryCandidateIntersectionTriangleEXT &&
				ray_query_alpha_test(
						rayQueryGetIntersectionInstanceCustomIndexEXT(query, false),
						rayQueryGetIntersectionPrimitiveIndexEXT(query, false),
						rayQueryGetIntersectionBarycentricsEXT(query, false))) {
			rayQueryConfirmIntersectionEXT(query);
		}
	}

	if (rayQueryGetIntersectionTypeEXT(query, true) ==
			gl_RayQueryCommittedIntersectionNoneEXT) {
		store_probe_ray(
				probe_index,
				ray_index,
				sample_sky(ray_direction),
				ddgi_grid.max_ray_distance);
		return;
	}

	float hit_t = rayQueryGetIntersectionTEXT(query, true);
	bool front_face = rayQueryGetIntersectionFrontFaceEXT(query, true);
	if (!front_face) {
		// Keep backfaces strongly occluding without using relocation/classification yet.
		store_probe_ray(probe_index, ray_index, vec3(0.0), -max(hit_t * 0.2, 0.001));
		return;
	}

	uint geometry_index = rayQueryGetIntersectionInstanceCustomIndexEXT(query, true);
	uint primitive_index = rayQueryGetIntersectionPrimitiveIndexEXT(query, true);
	vec2 hit_barycentrics = rayQueryGetIntersectionBarycentricsEXT(query, true);
	vec3 barycentrics =
			vec3(1.0 - hit_barycentrics.x - hit_barycentrics.y, hit_barycentrics);

	GeometryData geometry = geometries[geometry_index];
	uint i0;
	uint i1;
	uint i2;
	get_triangle_indices_ex(geometry, primitive_index, i0, i1, i2);

	vec2 material_uv = fetch_uv(geometry, i0, i1, i2, barycentrics);
	TBNResult object_tbn = fetch_tbn(geometry, i0, i1, i2, barycentrics);

	mat4x3 object_to_world = rayQueryGetIntersectionObjectToWorldEXT(query, true);
	mat4x3 world_to_object = rayQueryGetIntersectionWorldToObjectEXT(query, true);
	mat3 tangent_matrix = mat3(
			object_to_world[0].xyz,
			object_to_world[1].xyz,
			object_to_world[2].xyz);
	mat3 normal_matrix = transpose(mat3(
			world_to_object[0].xyz,
			world_to_object[1].xyz,
			world_to_object[2].xyz));

	vec3 geometry_normal = normalize(normal_matrix * object_tbn.normal);
	vec3 tangent = normalize(tangent_matrix * object_tbn.tangent);
	vec3 bitangent =
			normalize(cross(geometry_normal, tangent)) * object_tbn.bitangent_sign;

	MaterialData material = materials[geometry_index];
	material_uv = material_uv * material.uv1_scale + material.uv1_offset;

	vec3 albedo =
			sample_material_texture(material.albedo_texture_idx, material_uv, material.flags).rgb *
			material.albedo_color.rgb;
	vec3 orm =
			sample_material_texture(material.orm_texture_idx, material_uv, material.flags).rgb;
	float ao = mix(1.0, orm.r, material.ao_strength);
	float roughness = clamp(orm.g * material.roughness, 0.0, 1.0);
	float metalness = clamp(orm.b * material.metallic, 0.0, 1.0);

	vec3 shading_normal = geometry_normal;
	if ((material.flags & 1u) != 0u) {
		vec3 tangent_normal;
		tangent_normal.xy =
				sample_material_texture(
						material.normal_texture_idx,
						material_uv,
						material.flags)
								.xy *
						2.0 -
				1.0;
		tangent_normal.z =
				sqrt(max(0.0, 1.0 - dot(tangent_normal.xy, tangent_normal.xy)));
		vec3 mapped_normal = normalize(
				tangent * tangent_normal.x +
				bitangent * tangent_normal.y +
				geometry_normal * tangent_normal.z);
		shading_normal =
				normalize(mix(geometry_normal, mapped_normal, material.normal_map_depth));
	}

	vec3 world_position = ray_origin + ray_direction * hit_t;
	vec3 biased_position = offset_ray_origin(
			world_position + geometry_normal * ddgi_grid.normal_bias -
					ray_direction * ddgi_grid.view_bias,
			geometry_normal);

	vec3 diffuse_albedo = albedo * (1.0 - metalness);
	MaterialProperties diffuse_material;
	diffuse_material.baseColor = diffuse_albedo;
	diffuse_material.metalness = 0.0;
	diffuse_material.emissive = vec3(0.0);
	diffuse_material.roughness = roughness;
	diffuse_material.dielectricF0 = 0.0;
	diffuse_material.transmissivness = 0.0;
	diffuse_material.opacity = 1.0;

	uint rng_state =
			init_rng(uvec2(ray_index, probe_index), ddgi_grid.frame_index, 0u);
	vec3 radiance = lights_evaluate_direct_lighting(
			biased_position,
			shading_normal,
			-ray_direction,
			diffuse_material,
			rng_state,
			true,
			params.light_count);

	if ((material.flags & 2u) != 0u) {
		vec3 emission =
				sample_material_texture(
						material.emission_texture_idx,
						material_uv,
						material.flags)
						.rgb *
				material.emission_color *
				material.emission_strength;
		radiance += emission * scene_data_block.data.emissive_exposure_normalization;
	}

	if (ddgi_grid.history_reset == 0u) {
		float previous_weight;
		vec3 previous_irradiance = ddgi_sample_irradiance(
				previous_irradiance_atlas,
				previous_distance_atlas,
				ddgi_linear_sampler,
				ddgi_pre_grid_position(),
				world_position,
				geometry_normal,
				shading_normal,
				ray_direction,
				previous_weight);
		radiance +=
				min(diffuse_albedo, vec3(0.9)) * previous_irradiance * (ao / PI);
	}

	store_probe_ray(probe_index, ray_index, radiance, hit_t);
}
