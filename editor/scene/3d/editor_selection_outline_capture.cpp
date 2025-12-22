/**************************************************************************/
/*  editor_selection_outline_capture.cpp                                  */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
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

#include "editor_selection_outline_capture.h"

#include "core/config/engine.h"
#include "core/object/callable_mp.h"
#include "scene/resources/compositor.h"
#include "servers/rendering/renderer_rd/storage_rd/render_data_rd.h"
#include "servers/rendering/renderer_rd/storage_rd/render_scene_buffers_rd.h"
#include "servers/rendering/rendering_server.h"

static const char *capture_vertex_glsl = R"(#version 450
void main() {
	vec2 uv = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
	gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);
}
)";

static const char *capture_fragment_glsl = R"(#version 450
layout(push_constant, std430) uniform Params {
	vec4 value;
} params;

layout(location = 0) out vec4 out_color;

void main() {
	out_color = params.value;
}
)";

static const char *dilate_fragment_glsl = R"(#version 450
layout(push_constant, std430) uniform Params {
	vec2 axis;
	float radius;
	float mode; // 0 = dilate, 1 = erode, 2 = blur.
} params;

layout(set = 0, binding = 0) uniform sampler2D src;

layout(location = 0) out vec4 out_color;

void main() {
	ivec2 pos = ivec2(gl_FragCoord.xy);
	ivec2 last = textureSize(src, 0) - ivec2(1);
	ivec2 axis = ivec2(params.axis);
	vec3 result;
	if (params.mode > 1.5) {
		// 5-tap Gaussian, giving the mask the coverage gradients the outline
		// shader needs for antialiasing, independent of the MSAA setting.
		result = texelFetch(src, pos, 0).rgb * 0.375;
		result += (texelFetch(src, clamp(pos + axis, ivec2(0), last), 0).rgb + texelFetch(src, clamp(pos - axis, ivec2(0), last), 0).rgb) * 0.25;
		result += (texelFetch(src, clamp(pos + axis * 2, ivec2(0), last), 0).rgb + texelFetch(src, clamp(pos - axis * 2, ivec2(0), last), 0).rgb) * 0.0625;
	} else {
		bool erode = params.mode > 0.5;
		vec3 center = texelFetch(src, pos, 0).rgb;
		result = center;
		int radius = int(params.radius);
		for (int i = 1; i <= radius; i++) {
			ivec2 pos_a = pos + axis * i;
			ivec2 pos_b = pos - axis * i;
			vec3 a = texelFetch(src, clamp(pos_a, ivec2(0), last), 0).rgb;
			vec3 b = texelFetch(src, clamp(pos_b, ivec2(0), last), 0).rgb;
			if (erode) {
				result = min(result, min(a, b));
			} else {
				// Off-screen counts as empty, so dilation can't smear the mask
				// along viewport borders in a way erosion can't undo.
				if (any(lessThan(pos_a, ivec2(0))) || any(greaterThan(pos_a, last))) {
					a = vec3(0.0);
				}
				if (any(lessThan(pos_b, ivec2(0))) || any(greaterThan(pos_b, last))) {
					b = vec3(0.0);
				}
				result = max(result, max(a, b));
			}
		}
		// The visible channel is never morphologically closed, or thin
		// occluders would stop dimming the outline; it only gets the blur.
		result.b = center.b;
	}
	out_color = vec4(result, 0.0);
}
)";

void EditorSelectionOutlineCapture::setup(RID p_camera) {
	RenderingServer *rs = RenderingServer::get_singleton();
	attached_camera = p_camera;

	compositor_effect = rs->compositor_effect_create();
	rs->compositor_effect_set_callback(compositor_effect, RSE::COMPOSITOR_EFFECT_CALLBACK_TYPE_POST_TRANSPARENT, callable_mp(this, &EditorSelectionOutlineCapture::_render_callback));
	rs->compositor_effect_set_enabled(compositor_effect, false);

	compositor = rs->compositor_create();
	TypedArray<RID> effects;
	effects.push_back(compositor_effect);
	rs->compositor_set_compositor_effects(compositor, effects);

	overlay_texture.instantiate();
}

void EditorSelectionOutlineCapture::set_extract_enabled(bool p_enabled) {
	if (!compositor_effect.is_valid() || p_enabled == extract_enabled) {
		return;
	}
	extract_enabled = p_enabled;

	if (p_enabled) {
		// The overlay must not show again until a capture for the new state
		// has actually been produced.
		mask_ready = false;
	}

	RenderingServer *rs = RenderingServer::get_singleton();
	rs->compositor_effect_set_enabled(compositor_effect, p_enabled);
	// A camera compositor overrides the scenario's compositor entirely, so only
	// stay attached while extracting; while attached, the scene's own effects
	// are carried along (see set_scene_compositor_effects).
	rs->camera_set_compositor(attached_camera, p_enabled ? compositor : RID());
}

void EditorSelectionOutlineCapture::set_scene_compositor_effects(const Ref<Compositor> &p_compositor) {
	if (!compositor.is_valid()) {
		return;
	}

	Vector<RID> rids;
	if (p_compositor.is_valid()) {
		TypedArray<CompositorEffect> effects = p_compositor->get_compositor_effects();
		for (int i = 0; i < effects.size(); i++) {
			Ref<CompositorEffect> effect = effects[i];
			if (effect.is_valid() && effect->get_rid().is_valid()) {
				rids.push_back(effect->get_rid());
			}
		}
	}
	if (rids == scene_effect_rids) {
		return;
	}
	scene_effect_rids = rids;

	TypedArray<RID> all_effects;
	for (const RID &rid : scene_effect_rids) {
		all_effects.push_back(rid);
	}
	all_effects.push_back(compositor_effect);
	RenderingServer::get_singleton()->compositor_set_compositor_effects(compositor, all_effects);
}

bool EditorSelectionOutlineCapture::_ensure_shader() {
	if (shader.is_valid()) {
		return true;
	}
	if (shader_failed) {
		return false;
	}

	RD *rd = RD::get_singleton();

	String error;
	Vector<RD::ShaderStageSPIRVData> stages;

	RD::ShaderStageSPIRVData vertex_stage;
	vertex_stage.shader_stage = RD::SHADER_STAGE_VERTEX;
	vertex_stage.spirv = rd->shader_compile_spirv_from_source(RD::SHADER_STAGE_VERTEX, capture_vertex_glsl, RD::SHADER_LANGUAGE_GLSL, &error);
	if (vertex_stage.spirv.is_empty()) {
		shader_failed = true;
		ERR_FAIL_V_MSG(false, "Selection outline capture vertex shader failed to compile: " + error);
	}
	stages.push_back(vertex_stage);

	RD::ShaderStageSPIRVData fragment_stage;
	fragment_stage.shader_stage = RD::SHADER_STAGE_FRAGMENT;
	fragment_stage.spirv = rd->shader_compile_spirv_from_source(RD::SHADER_STAGE_FRAGMENT, capture_fragment_glsl, RD::SHADER_LANGUAGE_GLSL, &error);
	if (fragment_stage.spirv.is_empty()) {
		shader_failed = true;
		ERR_FAIL_V_MSG(false, "Selection outline capture fragment shader failed to compile: " + error);
	}
	stages.push_back(fragment_stage);

	shader = rd->shader_create_from_spirv(stages, "EditorSelectionOutlineCapture");
	if (!shader.is_valid()) {
		shader_failed = true;
		return false;
	}
	return true;
}

bool EditorSelectionOutlineCapture::_ensure_dilate_shader() {
	if (dilate_shader.is_valid()) {
		return true;
	}
	if (dilate_shader_failed) {
		return false;
	}

	RD *rd = RD::get_singleton();

	String error;
	Vector<RD::ShaderStageSPIRVData> stages;

	RD::ShaderStageSPIRVData vertex_stage;
	vertex_stage.shader_stage = RD::SHADER_STAGE_VERTEX;
	vertex_stage.spirv = rd->shader_compile_spirv_from_source(RD::SHADER_STAGE_VERTEX, capture_vertex_glsl, RD::SHADER_LANGUAGE_GLSL, &error);
	if (vertex_stage.spirv.is_empty()) {
		dilate_shader_failed = true;
		ERR_FAIL_V_MSG(false, "Selection outline dilate vertex shader failed to compile: " + error);
	}
	stages.push_back(vertex_stage);

	RD::ShaderStageSPIRVData fragment_stage;
	fragment_stage.shader_stage = RD::SHADER_STAGE_FRAGMENT;
	fragment_stage.spirv = rd->shader_compile_spirv_from_source(RD::SHADER_STAGE_FRAGMENT, dilate_fragment_glsl, RD::SHADER_LANGUAGE_GLSL, &error);
	if (fragment_stage.spirv.is_empty()) {
		dilate_shader_failed = true;
		ERR_FAIL_V_MSG(false, "Selection outline dilate fragment shader failed to compile: " + error);
	}
	stages.push_back(fragment_stage);

	dilate_shader = rd->shader_create_from_spirv(stages, "EditorSelectionOutlineDilate");
	if (!dilate_shader.is_valid()) {
		dilate_shader_failed = true;
		return false;
	}
	return true;
}

void EditorSelectionOutlineCapture::_process_mask(int p_radius, bool p_buffers_changed) {
	if (!_ensure_dilate_shader()) {
		return;
	}

	RD *rd = RD::get_singleton();

	if (dilate_sampler.is_null()) {
		dilate_sampler = rd->sampler_create(RD::SamplerState());
	}

	// dilate_size can lag mask_size if the viewport was resized while gap fill
	// was disabled, so it has to be compared explicitly.
	if (p_buffers_changed || !dilate_texture.is_valid() || dilate_size != mask_size) {
		// The fbs and uniform sets referencing the old textures may already have
		// been dependency-freed; only free what is still alive.
		if (dilate_src_mask_set.is_valid() && rd->uniform_set_is_valid(dilate_src_mask_set)) {
			rd->free_rid(dilate_src_mask_set);
		}
		dilate_src_mask_set = RID();
		if (dilate_src_dilate_set.is_valid() && rd->uniform_set_is_valid(dilate_src_dilate_set)) {
			rd->free_rid(dilate_src_dilate_set);
		}
		dilate_src_dilate_set = RID();
		if (dilate_fb.is_valid() && rd->framebuffer_is_valid(dilate_fb)) {
			rd->free_rid(dilate_fb);
		}
		dilate_fb = RID();
		if (dilate_back_fb.is_valid() && rd->framebuffer_is_valid(dilate_back_fb)) {
			rd->free_rid(dilate_back_fb);
		}
		dilate_back_fb = RID();
		if (dilate_texture.is_valid()) {
			rd->free_rid(dilate_texture);
		}
		dilate_texture = RID();

		RD::TextureFormat tf;
		tf.format = RD::DATA_FORMAT_R8G8B8A8_UNORM;
		tf.width = mask_size.x;
		tf.height = mask_size.y;
		tf.usage_bits = RD::TEXTURE_USAGE_COLOR_ATTACHMENT_BIT | RD::TEXTURE_USAGE_SAMPLING_BIT;
		dilate_texture = rd->texture_create(tf, RD::TextureView());
		ERR_FAIL_COND(!dilate_texture.is_valid());
		dilate_size = mask_size;
	}

	if (dilate_fb.is_valid() && !rd->framebuffer_is_valid(dilate_fb)) {
		dilate_fb = RID();
	}
	if (!dilate_fb.is_valid()) {
		dilate_fb = rd->framebuffer_create({ dilate_texture });
		ERR_FAIL_COND(!dilate_fb.is_valid());
	}
	if (dilate_back_fb.is_valid() && !rd->framebuffer_is_valid(dilate_back_fb)) {
		dilate_back_fb = RID();
	}
	if (!dilate_back_fb.is_valid()) {
		dilate_back_fb = rd->framebuffer_create({ mask_texture });
		ERR_FAIL_COND(!dilate_back_fb.is_valid());
	}

	const RD::FramebufferFormatID dilate_format = rd->framebuffer_get_format(dilate_fb);
	if (dilate_format != dilate_pipeline_format || dilate_pipeline.is_null()) {
		if (dilate_pipeline.is_valid()) {
			rd->free_rid(dilate_pipeline);
		}
		RD::PipelineRasterizationState raster_state;
		raster_state.cull_mode = RD::POLYGON_CULL_DISABLED;
		dilate_pipeline = rd->render_pipeline_create(dilate_shader, dilate_format, RD::INVALID_FORMAT_ID, RD::RENDER_PRIMITIVE_TRIANGLES, raster_state, RD::PipelineMultisampleState(), RD::PipelineDepthStencilState(), RD::PipelineColorBlendState::create_disabled(1));
		ERR_FAIL_COND(!dilate_pipeline.is_valid());
		dilate_pipeline_format = dilate_format;
	}

	if (dilate_src_mask_set.is_valid() && !rd->uniform_set_is_valid(dilate_src_mask_set)) {
		dilate_src_mask_set = RID();
	}
	if (!dilate_src_mask_set.is_valid()) {
		Vector<RD::Uniform> uniforms;
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE;
		u.binding = 0;
		u.append_id(dilate_sampler);
		u.append_id(mask_texture);
		uniforms.push_back(u);
		dilate_src_mask_set = rd->uniform_set_create(uniforms, dilate_shader, 0);
		ERR_FAIL_COND(!dilate_src_mask_set.is_valid());
	}
	if (dilate_src_dilate_set.is_valid() && !rd->uniform_set_is_valid(dilate_src_dilate_set)) {
		dilate_src_dilate_set = RID();
	}
	if (!dilate_src_dilate_set.is_valid()) {
		Vector<RD::Uniform> uniforms;
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE;
		u.binding = 0;
		u.append_id(dilate_sampler);
		u.append_id(dilate_texture);
		uniforms.push_back(u);
		dilate_src_dilate_set = rd->uniform_set_create(uniforms, dilate_shader, 0);
		ERR_FAIL_COND(!dilate_src_dilate_set.is_valid());
	}

	// Morphological closing (dilate merges the gaps, the following erosion
	// returns the outer silhouette to its original position), then a small
	// blur so the outline shader always has smooth coverage to antialias from.
	float pass_params[6][4];
	int pass_count = 0;
	if (p_radius > 0) {
		const float closing[4][4] = {
			{ 1.0f, 0.0f, float(p_radius), 0.0f },
			{ 0.0f, 1.0f, float(p_radius), 0.0f },
			{ 1.0f, 0.0f, float(p_radius), 1.0f },
			{ 0.0f, 1.0f, float(p_radius), 1.0f },
		};
		for (int i = 0; i < 4; i++) {
			memcpy(pass_params[pass_count++], closing[i], sizeof(float[4]));
		}
	}
	const float blur[2][4] = {
		{ 1.0f, 0.0f, 0.0f, 2.0f },
		{ 0.0f, 1.0f, 0.0f, 2.0f },
	};
	for (int i = 0; i < 2; i++) {
		memcpy(pass_params[pass_count++], blur[i], sizeof(float[4]));
	}

	for (int i = 0; i < pass_count; i++) {
		const bool to_dilate_texture = (i % 2) == 0;
		RD::DrawListID dilate_list = rd->draw_list_begin(to_dilate_texture ? dilate_fb : dilate_back_fb, RD::DRAW_IGNORE_COLOR_0);
		rd->draw_list_bind_render_pipeline(dilate_list, dilate_pipeline);
		rd->draw_list_bind_uniform_set(dilate_list, to_dilate_texture ? dilate_src_mask_set : dilate_src_dilate_set, 0);
		rd->draw_list_set_push_constant(dilate_list, pass_params[i], sizeof(float[4]));
		rd->draw_list_draw(dilate_list, false, 1, 3);
		rd->draw_list_end();
	}
}

void EditorSelectionOutlineCapture::_render_callback(int p_callback_type, RenderData *p_render_data) {
	RD *rd = RD::get_singleton();
	if (!rd) {
		return;
	}

	RenderDataRD *render_data = Object::cast_to<RenderDataRD>(p_render_data);
	if (!render_data) {
		return;
	}

	Ref<RenderSceneBuffersRD> rb = render_data->render_buffers;
	if (rb.is_null() || rb->get_view_count() != 1 || !rb->has_depth_texture()) {
		return;
	}

	const Size2i size = rb->get_internal_size();
	if (size.x < 1 || size.y < 1) {
		return;
	}

	const bool use_msaa = rb->get_msaa_3d() != RSE::VIEWPORT_MSAA_DISABLED && rb->has_texture(RB_SCOPE_BUFFERS, RB_TEX_DEPTH_MSAA);
	const RID depth = use_msaa ? rb->get_depth_msaa() : rb->get_depth_texture();
	const RD::TextureSamples samples = use_msaa ? rb->get_texture_samples() : RD::TEXTURE_SAMPLES_1;
	if (!depth.is_valid()) {
		return;
	}

	if (!_ensure_shader()) {
		return;
	}

	const bool buffers_changed = size != mask_size || samples != mask_samples;
	if (framebuffer.is_valid() && !rd->framebuffer_is_valid(framebuffer)) {
		// Already freed by RD when an attachment (the scene depth) was freed on resize.
		framebuffer = RID();
	}
	if (framebuffer.is_valid() && (buffers_changed || depth != framebuffer_depth)) {
		rd->free_rid(framebuffer);
		framebuffer = RID();
	}

	if (buffers_changed || !mask_texture.is_valid()) {
		// The overlay may still be sampling the old mask this frame; retire it
		// on the main thread after the overlay has been pointed at the new one.
		Array retired;
		if (mask_texture.is_valid()) {
			retired.push_back(mask_texture);
		}
		if (mask_texture_msaa.is_valid()) {
			retired.push_back(mask_texture_msaa);
		}
		mask_texture = RID();
		mask_texture_msaa = RID();

		RD::TextureFormat tf;
		tf.format = RD::DATA_FORMAT_R8G8B8A8_UNORM;
		tf.width = size.x;
		tf.height = size.y;
		tf.usage_bits = RD::TEXTURE_USAGE_COLOR_ATTACHMENT_BIT | RD::TEXTURE_USAGE_SAMPLING_BIT;
		mask_texture = rd->texture_create(tf, RD::TextureView());
		ERR_FAIL_COND(!mask_texture.is_valid());

		if (samples != RD::TEXTURE_SAMPLES_1) {
			tf.samples = samples;
			tf.usage_bits = RD::TEXTURE_USAGE_COLOR_ATTACHMENT_BIT;
			mask_texture_msaa = rd->texture_create(tf, RD::TextureView());
			ERR_FAIL_COND(!mask_texture_msaa.is_valid());
		}

		mask_size = size;
		mask_samples = samples;

		callable_mp(this, &EditorSelectionOutlineCapture::_assign_overlay_texture).call_deferred(mask_texture, retired);
	}

	if (!framebuffer.is_valid()) {
		Vector<RID> fb_textures;
		RD::FramebufferPass pass;
		if (mask_texture_msaa.is_valid()) {
			fb_textures = { mask_texture_msaa, depth, mask_texture };
			pass.color_attachments = { 0 };
			pass.depth_attachment = 1;
			pass.resolve_attachments = { 2 };
		} else {
			fb_textures = { mask_texture, depth };
			pass.color_attachments = { 0 };
			pass.depth_attachment = 1;
		}
		framebuffer = rd->framebuffer_create_multipass(fb_textures, { pass });
		ERR_FAIL_COND(!framebuffer.is_valid());
		framebuffer_depth = depth;
	}

	const RD::FramebufferFormatID fb_format = rd->framebuffer_get_format(framebuffer);
	if (fb_format != pipelines_format) {
		const int stencil_refs[4] = { STENCIL_REF_SELECTED, STENCIL_REF_SELECTED_VISIBLE, STENCIL_REF_ACTIVE, STENCIL_REF_ACTIVE_VISIBLE };
		for (int i = 0; i < 4; i++) {
			if (pipelines[i].is_valid()) {
				rd->free_rid(pipelines[i]);
			}

			RD::PipelineRasterizationState raster_state;
			raster_state.cull_mode = RD::POLYGON_CULL_DISABLED;

			RD::PipelineMultisampleState multisample_state;
			multisample_state.sample_count = mask_samples;

			// The stencil test against the scene's stencil buffer does the
			// "reading": the quad only covers pixels whose stencil equals the
			// outline mask reference, so no stencil sampling is needed.
			RD::PipelineDepthStencilState depth_stencil_state;
			depth_stencil_state.enable_depth_test = false;
			depth_stencil_state.enable_depth_write = false;
			depth_stencil_state.enable_stencil = true;
			RD::PipelineDepthStencilState::StencilOperationState op;
			op.fail = RD::STENCIL_OP_KEEP;
			op.pass = RD::STENCIL_OP_KEEP;
			op.depth_fail = RD::STENCIL_OP_KEEP;
			op.compare = RD::COMPARE_OP_EQUAL;
			op.compare_mask = 0xFF;
			op.write_mask = 0;
			op.reference = stencil_refs[i];
			depth_stencil_state.front_op = op;
			depth_stencil_state.back_op = op;

			pipelines[i] = rd->render_pipeline_create(shader, fb_format, RD::INVALID_FORMAT_ID, RD::RENDER_PRIMITIVE_TRIANGLES, raster_state, multisample_state, depth_stencil_state, RD::PipelineColorBlendState::create_disabled(1));
			ERR_FAIL_COND(!pipelines[i].is_valid());
		}
		pipelines_format = fb_format;
	}

	Vector<Color> clear_colors = { Color(0, 0, 0, 0) };
	RD::DrawListID draw_list = rd->draw_list_begin(framebuffer, RD::DRAW_CLEAR_COLOR_0, clear_colors);
	// R encodes presence, G the active node, B the visible (unoccluded) part.
	const float values[4][4] = {
		{ 1.0f, 0.0f, 0.0f, 0.0f },
		{ 1.0f, 0.0f, 1.0f, 0.0f },
		{ 1.0f, 1.0f, 0.0f, 0.0f },
		{ 1.0f, 1.0f, 1.0f, 0.0f },
	};
	for (int i = 0; i < 4; i++) {
		rd->draw_list_bind_render_pipeline(draw_list, pipelines[i]);
		rd->draw_list_set_push_constant(draw_list, values[i], sizeof(float[4]));
		rd->draw_list_draw(draw_list, false, 1, 3);
	}
	rd->draw_list_end();

	// Merge gaps narrower than twice the radius (e.g. between the bars of a
	// fence) so the outline wraps the part instead of every thin feature,
	// and feather the mask for antialiasing. The radius is given in output
	// pixels but applied in mask texels, so account for 3D resolution scaling.
	const Size2i target_size = rb->get_target_size();
	const float resolution_scale = target_size.x > 0 ? float(size.x) / float(target_size.x) : 1.0f;
	_process_mask(CLAMP(int(Math::round(gap_fill_radius * resolution_scale)), 0, 32), buffers_changed);

	last_capture_frame = Engine::get_singleton()->get_frames_drawn();

	if (!mask_ready) {
		// Only flag readiness after a successful capture, so the overlay never
		// shows placeholder or stale data. Editor rendering runs on the main
		// thread, making the deferred call ordering reliable.
		callable_mp(this, &EditorSelectionOutlineCapture::_mark_mask_ready).call_deferred();
	}
}

bool EditorSelectionOutlineCapture::is_capture_fresh() const {
	return Engine::get_singleton()->get_frames_drawn() <= last_capture_frame + 2;
}

void EditorSelectionOutlineCapture::_assign_overlay_texture(RID p_mask, const Array &p_retired) {
	if (overlay_texture.is_valid()) {
		overlay_texture->set_texture_rd_rid(p_mask);
	}
	if (!p_retired.is_empty()) {
		RenderingServer::get_singleton()->call_on_render_thread(callable_mp_static(&EditorSelectionOutlineCapture::_free_rd_rids).bind(p_retired));
	}
}

void EditorSelectionOutlineCapture::_mark_mask_ready() {
	mask_ready = true;
	if (mask_changed_callback.is_valid()) {
		mask_changed_callback.call();
	}
}

void EditorSelectionOutlineCapture::_free_rd_rids(const Array &p_rids) {
	RD *rd = RD::get_singleton();
	if (!rd) {
		return;
	}
	for (int i = 0; i < p_rids.size(); i++) {
		RID rid = p_rids[i];
		if (rid.is_valid()) {
			rd->free_rid(rid);
		}
	}
}

EditorSelectionOutlineCapture::~EditorSelectionOutlineCapture() {
	RenderingServer *rs = RenderingServer::get_singleton();
	if (rs) {
		if (attached_camera.is_valid()) {
			rs->camera_set_compositor(attached_camera, RID());
		}
		if (compositor.is_valid()) {
			rs->free_rid(compositor);
		}
		if (compositor_effect.is_valid()) {
			rs->free_rid(compositor_effect);
		}

		if (RD::get_singleton()) {
			// The overlay texture wrapper is released before its backing RD
			// texture, which is freed on the render thread.
			if (overlay_texture.is_valid()) {
				overlay_texture->set_texture_rd_rid(RID());
			}
			RD *rd = RD::get_singleton();
			// Dependent resources (uniform sets, framebuffers) first, so freeing
			// their textures doesn't cascade onto entries later in the list.
			Array rids;
			if (dilate_src_mask_set.is_valid() && rd->uniform_set_is_valid(dilate_src_mask_set)) {
				rids.push_back(dilate_src_mask_set);
			}
			if (dilate_src_dilate_set.is_valid() && rd->uniform_set_is_valid(dilate_src_dilate_set)) {
				rids.push_back(dilate_src_dilate_set);
			}
			if (framebuffer.is_valid() && rd->framebuffer_is_valid(framebuffer)) {
				rids.push_back(framebuffer);
			}
			if (dilate_fb.is_valid() && rd->framebuffer_is_valid(dilate_fb)) {
				rids.push_back(dilate_fb);
			}
			if (dilate_back_fb.is_valid() && rd->framebuffer_is_valid(dilate_back_fb)) {
				rids.push_back(dilate_back_fb);
			}
			for (int i = 0; i < 4; i++) {
				if (pipelines[i].is_valid()) {
					rids.push_back(pipelines[i]);
				}
			}
			if (dilate_pipeline.is_valid()) {
				rids.push_back(dilate_pipeline);
			}
			if (mask_texture.is_valid()) {
				rids.push_back(mask_texture);
			}
			if (mask_texture_msaa.is_valid()) {
				rids.push_back(mask_texture_msaa);
			}
			if (dilate_texture.is_valid()) {
				rids.push_back(dilate_texture);
			}
			if (dilate_sampler.is_valid()) {
				rids.push_back(dilate_sampler);
			}
			if (shader.is_valid()) {
				rids.push_back(shader);
			}
			if (dilate_shader.is_valid()) {
				rids.push_back(dilate_shader);
			}
			if (!rids.is_empty()) {
				rs->call_on_render_thread(callable_mp_static(&EditorSelectionOutlineCapture::_free_rd_rids).bind(rids));
			}
		}
	}
}
