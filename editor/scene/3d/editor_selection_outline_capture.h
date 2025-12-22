/**************************************************************************/
/*  editor_selection_outline_capture.h                                    */
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

#pragma once

#include "core/object/ref_counted.h"
#include "scene/resources/texture_rd.h"
#include "servers/rendering/rendering_device.h"

class Compositor;
class RenderData;

// Extracts the selection stencil mask that the 3D editor's outline materials
// stamp into the scene's stencil buffer into a small color texture
// (R = selected, G = active, B = visible), as a compositor pass that runs
// inside the editor camera's frame render. The viewport's outline overlay
// then edge-detects that mask in 2D, so the screen-space cost is fixed and
// the only per-object cost is the flat stencil-write passes.
class EditorSelectionOutlineCapture : public RefCounted {
	GDCLASS(EditorSelectionOutlineCapture, RefCounted);

public:
	// High values to stay clear of the low stencil references scene materials
	// typically use. The "visible" variants are written by a depth-tested pass
	// after their depth-ignoring counterpart, so they win where the geometry is
	// actually in front.
	enum StencilRef {
		STENCIL_REF_SELECTED = 250,
		STENCIL_REF_SELECTED_VISIBLE = 251,
		STENCIL_REF_ACTIVE = 252,
		STENCIL_REF_ACTIVE_VISIBLE = 253,
	};

private:
	RID compositor_effect;
	RID compositor;
	RID attached_camera;
	Vector<RID> scene_effect_rids;

	// Render-thread state. Only the compositor callback and the render-thread
	// free helper touch these, with the exception of the destructor which runs
	// after the effect has stopped being scheduled.
	RID shader;
	RID pipelines[4];
	RD::FramebufferFormatID pipelines_format = RD::INVALID_FORMAT_ID;
	RID framebuffer;
	RID mask_texture;
	RID mask_texture_msaa;
	RID framebuffer_depth;
	Size2i mask_size;
	RD::TextureSamples mask_samples = RD::TEXTURE_SAMPLES_1;
	bool shader_failed = false;

	RID dilate_shader;
	RID dilate_pipeline;
	RD::FramebufferFormatID dilate_pipeline_format = RD::INVALID_FORMAT_ID;
	RID dilate_texture;
	RID dilate_fb;
	RID dilate_back_fb;
	RID dilate_sampler;
	RID dilate_src_mask_set;
	RID dilate_src_dilate_set;
	Size2i dilate_size;
	bool dilate_shader_failed = false;
	int gap_fill_radius = 0;
	bool extract_enabled = false;

	Ref<Texture2DRD> overlay_texture;
	bool mask_ready = false;
	uint64_t last_capture_frame = 0;
	Callable mask_changed_callback;

	void _render_callback(int p_callback_type, RenderData *p_render_data);
	bool _ensure_shader();
	bool _ensure_dilate_shader();
	void _process_mask(int p_radius, bool p_buffers_changed);
	void _assign_overlay_texture(RID p_mask, const Array &p_retired);
	void _mark_mask_ready();
	static void _free_rd_rids(const Array &p_rids);

protected:
	static void _bind_methods() {}

public:
	void setup(RID p_camera);
	void set_extract_enabled(bool p_enabled);
	void set_scene_compositor_effects(const Ref<Compositor> &p_compositor);
	Ref<Texture2D> get_overlay_texture() const { return overlay_texture; }
	// Whether the overlay texture points at an extracted mask, so the overlay
	// is never drawn with a placeholder.
	bool has_mask() const { return mask_ready; }
	bool is_capture_fresh() const;
	void set_mask_changed_callback(const Callable &p_callback) { mask_changed_callback = p_callback; }
	void set_gap_fill_radius(int p_pixels) { gap_fill_radius = p_pixels; }

	~EditorSelectionOutlineCapture();
};
