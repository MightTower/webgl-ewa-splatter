var vertShader =
`#version 300 es
#line 4
layout(location=0) in vec3 pos;
layout(location=1) in highp vec4 splat_pos_radius;
layout(location=2) in highp vec4 splat_normal;
layout(location=3) in highp vec4 splat_color_in;

uniform bool depth_prepass;
uniform highp vec3 eye_pos;
uniform mat4 proj_view;
uniform float radius_scale;
uniform float scaling;

out highp vec2 uv;
flat out highp vec3 normal;
flat out highp vec3 splat_color;

mat3 rotation_matrix(vec3 a, float angle) {
	float c = cos(angle);
	float sub_c = 1.0 - c;
	float s = sin(angle);
	return mat3(
		vec3(c + pow(a.x, 2.0) * sub_c,
			a.y * a.x * sub_c + a.z * s,
			a.z * a.x * sub_c - a.y * s),
		vec3(a.x * a.y * sub_c - a.z * s,
			c+ pow(a.y, 2.0) * sub_c,
			a.z * a.y * sub_c + a.x * s),
		vec3(a.x * a.z * sub_c + a.y * s,
			a.y * a.z * sub_c - a.x * s,
			c + pow(a.z, 2.0) * sub_c));
}

void main(void) {
	mat3 rot_mat = mat3(1.0);
	vec3 quad_normal = vec3(0, 0, 1);
	float scaled_radius = splat_pos_radius.w * radius_scale * scaling;
	uv = 2.0 * pos.xy;
	normal = normalize(splat_normal.xyz);
	splat_color = splat_color_in.xyz;
	if (abs(normal) != quad_normal) {
		vec3 rot_axis = normalize(cross(quad_normal, normal));
		float rot_angle = acos(dot(quad_normal, normal));
		rot_mat = rotation_matrix(rot_axis, rot_angle);
	}
	vec3 sp = rot_mat * scaled_radius * pos + splat_pos_radius.xyz * scaling;
	vec3 view_dir = normalize(sp - eye_pos);
	if (depth_prepass) {
		sp += view_dir * scaled_radius * 0.5;
	}
	gl_Position = proj_view * vec4(sp, 1.0);
}`

var fragShader =
`#version 300 es
#line 58
precision highp int;
precision highp float;\n
#define M_PI 3.1415926535897932384626433832795\n

uniform bool depth_prepass;
in highp vec2 uv;
flat in highp vec3 normal;
flat in highp vec3 splat_color;

layout(location=0) out highp vec4 color;
layout(location=1) out highp vec3 normal_out;

void main(void) {
	highp float len = length(uv);
	if (len > 1.0) {
		discard;
	}
	if (!depth_prepass) {
		highp float opacity = 1.0 / sqrt(2.0 * M_PI) * exp(-pow(len * 2.5, 2.0)/2.0);
		color = vec4(splat_color * opacity, opacity);
		normal_out = opacity * normal;
	}
}`;

var quadVertShader =
`#version 300 es
#line 85
const vec4 pos[4] = vec4[4](
	vec4(-1, 1, 0.5, 1),
	vec4(-1, -1, 0.5, 1),
	vec4(1, 1, 0.5, 1),
	vec4(1, -1, 0.5, 1)
);
void main(void){
	gl_Position = pos[gl_VertexID];
}`;

var normalizationFragShader =
`#version 300 es
#line 98
precision highp int;
precision highp float;
uniform sampler2D splat_colors;
uniform sampler2D splat_normals;
uniform highp vec3 eye_dir;
out highp vec4 color;

float linear_to_srgb(float x) {
	if (x <= 0.0031308) {
		return 12.92 * x;
	}
	return 1.055 * pow(x, 1.0/2.4) - 0.055;
}

void main(void){ 
	ivec2 uv = ivec2(gl_FragCoord.xy);
	color = texelFetch(splat_colors, uv, 0);
	if (color.a != 0.0) {
		color.rgb = color.rgb / color.a;
		vec3 normal = normalize(texelFetch(splat_normals, uv, 0).xyz / color.a);
		vec3 light_dir = normalize(vec3(0.5, 0.5, 1));
		vec3 light_dir2 = normalize(vec3(-0.5, 0.25, -0.5));
		float intensity = 0.25;
		if (dot(light_dir, normal) > 0.0) {
			intensity += dot(light_dir, normal);
			highp vec3 h = normalize(normalize(-eye_dir) + light_dir);
			highp float ndoth = dot(h, normal);
			if (ndoth > 0.0) {
				intensity += pow(ndoth, 40.0);
			}
		}
		if (dot(light_dir2, normal) > 0.0) {
			intensity += dot(light_dir2, normal) * 0.5;
		}
		color.rgb *= intensity;
	} else {
		color.rgb = vec3(0.02);
	}
	color.r = linear_to_srgb(color.r);
	color.g = linear_to_srgb(color.g);
	color.b = linear_to_srgb(color.b);
	color.a = 1.0;
}`;

