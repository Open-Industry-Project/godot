/**************************************************************************/
/*  oip_time_scale_button.cpp                                             */
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

#include "oip_time_scale_button.h"

#include "core/config/engine.h"
#include "core/config/project_settings.h"
#include "core/object/callable_mp.h"
#include "editor/editor_string_names.h"

void OIPTimeScaleButton::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_READY: {
			connect(SNAME("toggled"), callable_mp(this, &OIPTimeScaleButton::_on_toggled));
			ProjectSettings::get_singleton()->connect(SNAME("settings_changed"), callable_mp(this, &OIPTimeScaleButton::_on_settings_changed));
		} break;
		case NOTIFICATION_THEME_CHANGED: {
			set_button_icon(get_theme_icon(SNAME("Time"), EditorStringName(EditorIcons)));
		} break;
	}
}

void OIPTimeScaleButton::_bind_methods() {
}

void OIPTimeScaleButton::_on_toggled(bool p_pressed) {
	float new_scale;
	if (last_known_scale < 1.0) {
		new_scale = 1.0;
	} else if (last_known_scale < 2.0) {
		new_scale = 2.0;
	} else if (last_known_scale < 4.0) {
		new_scale = 4.0;
	} else {
		new_scale = 1.0;
	}
	_set_time_scale(new_scale);
}

void OIPTimeScaleButton::_on_settings_changed() {
	Variant current_scale = ProjectSettings::get_singleton()->get_setting(TIME_SCALE_KEY, 1.0);
	float scale_val = current_scale;
	if (scale_val <= 0) {
		ERR_PRINT(vformat("Unsupported time scale (<= 0). Reverting to last known scale: %.2f", last_known_scale));
		_set_time_scale(last_known_scale);
		return;
	}
	if (scale_val != last_known_scale) {
		last_known_scale = scale_val;
		Engine::get_singleton()->set_time_scale(scale_val);
		_update_button_text();
	}
}

void OIPTimeScaleButton::_set_time_scale(float p_value) {
	last_known_scale = p_value;
	ProjectSettings::get_singleton()->set_setting(TIME_SCALE_KEY, p_value);
	ProjectSettings::get_singleton()->save();
	Engine::get_singleton()->set_time_scale(p_value);
	_update_button_text();
}

float OIPTimeScaleButton::_get_valid_time_scale() {
	if (ProjectSettings::get_singleton()->has_setting(TIME_SCALE_KEY)) {
		Variant saved_scale = ProjectSettings::get_singleton()->get_setting(TIME_SCALE_KEY);
		float scale_val = saved_scale;
		if (scale_val > 0) {
			return scale_val;
		}
		ERR_PRINT("Unsupported time scale (<= 0). Reverting to default 1.0.");
	}
	ProjectSettings::get_singleton()->set_setting(TIME_SCALE_KEY, 1.0);
	ProjectSettings::get_singleton()->save();
	return 1.0;
}

void OIPTimeScaleButton::_update_button_text() {
	set_text(vformat("%.2fx", last_known_scale));
	set_pressed_no_signal(last_known_scale != 1.0);
	queue_redraw();
}

OIPTimeScaleButton::OIPTimeScaleButton() {
	set_toggle_mode(true);
	set_tooltip_text("Change speed (1x, 2x, 4x)");
	set_flat(true);

	last_known_scale = _get_valid_time_scale();
	Engine::get_singleton()->set_time_scale(last_known_scale);
	_update_button_text();
}
