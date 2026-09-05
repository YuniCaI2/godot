#[compute]

#version 460

#VERSION_DEFINES 
//宏展开的位置

#extension GL_EXT_ray_query : enable
#extension GL_EXT_ray_tracing : enable

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

// Binding 0: 输出图像 (512x512 存储纹理)
layout(set = 0, binding = 0, rgba8) uniform restrict writeonly image2D output_image;

// Binding 1: 场景顶层加速结构 (TLAS)
layout(set = 0, binding = 1) uniform accelerationStructureEXT tlas;

void main() {
	ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
	if (pixel.x >= 512 || pixel.y >= 512) {
		return;
	}

	// 把像素坐标映射到 [-1.0, 1.0] 的相机视口空间
	vec2 uv = (vec2(pixel) + 0.5) / 512.0 * 2.0 - 1.0;

	// 定义一条从相机位置看向场景中心的光线
	vec3 ray_origin = vec3(0.0, 3.0, 5.0);
	vec3 ray_dir = normalize(vec3(uv.x, -uv.y, -1.5));

	// 初始化 RayQuery（内联光线查询）
	rayQueryEXT rq;
	rayQueryInitializeEXT(
		rq,
		tlas,
		gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsOpaqueEXT, // 只要打中任何物体立刻停下
		0xFF,                                                    // 遮罩 mask
		ray_origin,
		0.001,                                                   // t_min (微小偏置防自相交)
		ray_dir,
		1000.0                                                   // t_max
	);

	// 驱动光线遍历加速结构
	while (rayQueryProceedEXT(rq)) {
		// 这里由于开启了 Opaque + TerminateOnFirstHit，GPU 底层会自动确认命中
	}

	// 判断是否命中物体
	vec4 color;
	if (rayQueryGetIntersectionTypeEXT(rq, true) != gl_RayQueryCommittedIntersectionNoneEXT) {
		// 1. 获取光线击中物体的真实距离 (Hit Distance)
		float t = rayQueryGetIntersectionTEXT(rq, true);

		// 2. 将距离 [2.0米(近处球体) ~ 9.0米(远处墙壁)] 映射为 0~1 的深度渐变（近亮远暗）
		float depth_factor = clamp((t - 2.0) / 7.0, 0.0, 1.0);
		float shade = 1.0 - depth_factor;

		// 3. 读取击中三角形的重心坐标 (Barycentrics)，为每个三角形赋予 RGB 渐变，清晰看到网格几何结构
		vec2 bary = rayQueryGetIntersectionBarycentricsEXT(rq, true);
		vec3 bary_color = vec3(bary.x, bary.y, 1.0 - bary.x - bary.y);

		// 综合：距离明暗 + 三角形彩色结构
		color = vec4(mix(vec3(shade), bary_color, 0.4), 1.0);
	} else {
		color = vec4(0.05, 0.05, 0.05, 1.0); // 未打中（射向虚空）：深黑灰
	}

	imageStore(output_image, pixel, color);
}
