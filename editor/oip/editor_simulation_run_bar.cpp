/**************************************************************************/
/*  editor_simulation_run_bar.cpp                                         */
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

#include "editor_simulation_run_bar.h"

#include "core/object/callable_mp.h"
#include "editor/editor_interface.h"
#include "editor/editor_node.h"
#include "editor/editor_string_names.h"
#include "editor/settings/editor_settings.h"
#include "scene/gui/box_container.h"
#include "scene/gui/button.h"
#include "servers/physics_3d/physics_server_3d.h"

void EditorSimulationRunBar::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_THEME_CHANGED: {
			if (play_button) {
				play_button->set_button_icon(get_theme_icon(SNAME("Play"), EditorStringName(EditorIcons)));
				pause_button->set_button_icon(get_theme_icon(SNAME("Pause"), EditorStringName(EditorIcons)));
				stop_button->set_button_icon(get_theme_icon(SNAME("Stop"), EditorStringName(EditorIcons)));
				_update_shortcut_tooltips();
			}
		} break;
	}
}

void EditorSimulationRunBar::_bind_methods() {
	ADD_SIGNAL(MethodInfo("simulation_started"));
	ADD_SIGNAL(MethodInfo("simulation_stopped"));
	ADD_SIGNAL(MethodInfo("simulation_pause_toggled", PropertyInfo(Variant::BOOL, "paused")));
}

void EditorSimulationRunBar::_on_play_pressed() {
	start_simulation();
}

void EditorSimulationRunBar::_on_pause_toggled(bool p_pressed) {
	toggle_pause_simulation();
}

void EditorSimulationRunBar::_on_stop_pressed() {
	stop_simulation();
}

void EditorSimulationRunBar::_update_shortcut_tooltips() {
	Ref<EditorSettings> es = EditorInterface::get_singleton()->get_editor_settings();

	Ref<Shortcut> start_sc = es->get_shortcut("Open Industry Project/Start Simulation");
	Ref<Shortcut> pause_sc = es->get_shortcut("Open Industry Project/Toggle Pause Simulation");
	Ref<Shortcut> stop_sc = es->get_shortcut("Open Industry Project/Stop Simulation");

	if (start_sc.is_valid()) {
		play_button->set_shortcut(start_sc);
		play_button->set_shortcut_in_tooltip(false);
		play_button->set_tooltip_text(vformat("Start Simulation (%s)", start_sc->get_as_text()));
	}
	if (pause_sc.is_valid()) {
		pause_button->set_shortcut(pause_sc);
		pause_button->set_shortcut_in_tooltip(false);
		pause_button->set_tooltip_text(vformat("Toggle Pause Simulation (%s)", pause_sc->get_as_text()));
	}
	if (stop_sc.is_valid()) {
		stop_button->set_shortcut(stop_sc);
		stop_button->set_shortcut_in_tooltip(false);
		stop_button->set_tooltip_text(vformat("Stop Simulation (%s)", stop_sc->get_as_text()));
	}
}

void EditorSimulationRunBar::start_simulation() {
	simulation_running = true;
	simulation_paused = false;
	pause_button->set_pressed_no_signal(false);
	play_button->set_disabled(true);
	pause_button->set_disabled(false);
	stop_button->set_disabled(false);

	PhysicsServer3D::get_singleton()->set_active(true);
	EditorNode::get_singleton()->set_simulation_started(true);
	emit_signal(SNAME("simulation_started"));
}

void EditorSimulationRunBar::stop_simulation() {
	simulation_running = false;
	simulation_paused = false;
	pause_button->set_pressed_no_signal(false);
	play_button->set_disabled(false);
	pause_button->set_disabled(true);
	stop_button->set_disabled(true);

	PhysicsServer3D::get_singleton()->set_active(false);
	Node *edited_scene = EditorNode::get_singleton()->get_edited_scene();
	if (edited_scene) {
		edited_scene->set_process_mode(Node::PROCESS_MODE_INHERIT);
	}
	EditorNode::get_singleton()->set_simulation_started(false);
	emit_signal(SNAME("simulation_stopped"));
}

void EditorSimulationRunBar::toggle_pause_simulation() {
	simulation_paused = !simulation_paused;

	Node *edited_scene = EditorNode::get_singleton()->get_edited_scene();
	if (edited_scene) {
		edited_scene->set_process_mode(simulation_paused ? Node::PROCESS_MODE_DISABLED : Node::PROCESS_MODE_INHERIT);
	}

	emit_signal(SNAME("simulation_pause_toggled"), simulation_paused);
}

void EditorSimulationRunBar::enable_buttons() {
	if (simulation_running) {
		play_button->set_disabled(true);
		pause_button->set_disabled(false);
		stop_button->set_disabled(false);
	} else {
		play_button->set_disabled(false);
		pause_button->set_disabled(true);
		stop_button->set_disabled(true);
	}
}

void EditorSimulationRunBar::disable_buttons() {
	play_button->set_disabled(true);
	pause_button->set_disabled(true);
	stop_button->set_disabled(true);
}

EditorSimulationRunBar::EditorSimulationRunBar() {
	set_process_mode(Node::PROCESS_MODE_ALWAYS);
	set_mouse_filter(Control::MOUSE_FILTER_STOP);

	hbox = memnew(HBoxContainer);
	hbox->add_theme_constant_override(SNAME("separation"), 8);
	add_child(hbox);

	play_button = memnew(Button);
	play_button->set_tooltip_text("Start Simulation (F5)");
	play_button->connect(SceneStringName(pressed), callable_mp(this, &EditorSimulationRunBar::_on_play_pressed));
	hbox->add_child(play_button);

	pause_button = memnew(Button);
	pause_button->set_tooltip_text("Toggle Pause Simulation (F6)");
	pause_button->set_toggle_mode(true);
	pause_button->set_disabled(true);
	pause_button->connect(SNAME("toggled"), callable_mp(this, &EditorSimulationRunBar::_on_pause_toggled));
	hbox->add_child(pause_button);

	stop_button = memnew(Button);
	stop_button->set_tooltip_text("Stop Simulation (F7)");
	stop_button->set_disabled(true);
	stop_button->connect(SceneStringName(pressed), callable_mp(this, &EditorSimulationRunBar::_on_stop_pressed));
	hbox->add_child(stop_button);

	Ref<Shortcut> start_shortcut;
	start_shortcut.instantiate();
	Ref<InputEventKey> start_key;
	start_key.instantiate();
	start_key->set_keycode(Key::F5);
	start_shortcut->set_events({ start_key });
	EditorSettings::get_singleton()->add_shortcut("Open Industry Project/Start Simulation", start_shortcut);

	Ref<Shortcut> pause_shortcut;
	pause_shortcut.instantiate();
	Ref<InputEventKey> pause_key;
	pause_key.instantiate();
	pause_key->set_keycode(Key::F6);
	pause_shortcut->set_events({ pause_key });
	EditorSettings::get_singleton()->add_shortcut("Open Industry Project/Toggle Pause Simulation", pause_shortcut);

	Ref<Shortcut> stop_shortcut;
	stop_shortcut.instantiate();
	Ref<InputEventKey> stop_key;
	stop_key.instantiate();
	stop_key->set_keycode(Key::F7);
	stop_shortcut->set_events({ stop_key });
	EditorSettings::get_singleton()->add_shortcut("Open Industry Project/Stop Simulation", stop_shortcut);
}
