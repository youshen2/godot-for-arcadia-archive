/**************************************************************************/
/*  mobile_persistent_notification.cpp                                    */
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

#include "mobile_persistent_notification.h"

#include "core/object/class_db.h"
#include "core/os/os.h"

MobilePersistentNotification *MobilePersistentNotification::singleton = nullptr;

bool MobilePersistentNotification::is_supported() const {
	return OS::get_singleton()->is_mobile_persistent_notification_supported();
}

bool MobilePersistentNotification::is_active() const {
	return OS::get_singleton()->is_mobile_persistent_notification_active();
}

Error MobilePersistentNotification::start(const String &p_title, const String &p_message) {
	return OS::get_singleton()->show_mobile_persistent_notification(p_title, p_message);
}

Error MobilePersistentNotification::update(const String &p_title, const String &p_message) {
	if (!is_supported()) {
		return ERR_UNAVAILABLE;
	}
	ERR_FAIL_COND_V_MSG(!is_active(), ERR_UNCONFIGURED, "The mobile persistent notification is not active.");
	return OS::get_singleton()->update_mobile_persistent_notification(p_title, p_message);
}

void MobilePersistentNotification::stop() {
	OS::get_singleton()->hide_mobile_persistent_notification();
}

void MobilePersistentNotification::_bind_methods() {
	ClassDB::bind_method(D_METHOD("is_supported"), &MobilePersistentNotification::is_supported);
	ClassDB::bind_method(D_METHOD("is_active"), &MobilePersistentNotification::is_active);
	ClassDB::bind_method(D_METHOD("start", "title", "message"), &MobilePersistentNotification::start);
	ClassDB::bind_method(D_METHOD("update", "title", "message"), &MobilePersistentNotification::update);
	ClassDB::bind_method(D_METHOD("stop"), &MobilePersistentNotification::stop);
}

MobilePersistentNotification::MobilePersistentNotification() {
	singleton = this;
}

MobilePersistentNotification::~MobilePersistentNotification() {
	if (singleton == this) {
		singleton = nullptr;
	}
}
