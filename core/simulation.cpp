/**************************************************************************/
/*  simulation.cpp                                                        */
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

#include "simulation.h"

#include "core/object/class_db.h"

Simulation *Simulation::singleton = nullptr;

void Simulation::_bind_methods() {
	ClassDB::bind_method(D_METHOD("start"), &Simulation::start);
	ClassDB::bind_method(D_METHOD("stop"), &Simulation::stop);
	ClassDB::bind_method(D_METHOD("toggle_pause"), &Simulation::toggle_pause);
	ClassDB::bind_method(D_METHOD("is_running"), &Simulation::is_running);
	ClassDB::bind_method(D_METHOD("is_paused"), &Simulation::is_paused);

	ADD_SIGNAL(MethodInfo("started"));
	ADD_SIGNAL(MethodInfo("stopped"));
	ADD_SIGNAL(MethodInfo("pause_toggled", PropertyInfo(Variant::BOOL, "paused")));
}

void Simulation::start() {
	if (running) {
		return;
	}
	running = true;
	paused = false;
	emit_signal(SNAME("started"));
}

void Simulation::stop() {
	if (!running) {
		return;
	}
	running = false;
	paused = false;
	emit_signal(SNAME("stopped"));
}

void Simulation::toggle_pause() {
	if (!running) {
		return;
	}
	paused = !paused;
	emit_signal(SNAME("pause_toggled"), paused);
}

Simulation::Simulation() {
	singleton = this;
}
