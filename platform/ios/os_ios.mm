/**************************************************************************/
/*  os_ios.mm                                                             */
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

#import "os_ios.h"

#import "display_server_ios.h"

#ifdef IOS_ENABLED

OS_IOS *OS_IOS::get_singleton() {
	return (OS_IOS *)OS_AppleEmbedded::get_singleton();
}

OS_IOS::OS_IOS() :
		OS_AppleEmbedded() {
	DisplayServerIOS::register_ios_driver();
}

OS_IOS::~OS_IOS() {}

String OS_IOS::get_name() const {
	return "iOS";
}

bool OS_IOS::is_mobile_persistent_notification_supported() const {
	return true;
}

bool OS_IOS::is_mobile_persistent_notification_active() const {
	return mobile_persistent_notification_active;
}

Error OS_IOS::show_mobile_persistent_notification(const String &p_title, const String &p_message) {
	mobile_persistent_notification_title = p_title;
	mobile_persistent_notification_message = p_message;

	if (mobile_persistent_notification_task != UIBackgroundTaskInvalid) {
		mobile_persistent_notification_active = true;
		if (!is_app_focused()) {
			start_mobile_background_processing();
		}
		return OK;
	}

	NSString *task_name = [NSString stringWithUTF8String:p_title.utf8().get_data()];
	mobile_persistent_notification_task = [[UIApplication sharedApplication] beginBackgroundTaskWithName:task_name expirationHandler:^{
		OS_IOS *os_ios = OS_IOS::get_singleton();
		if (os_ios) {
			os_ios->hide_mobile_persistent_notification();
		}
	}];

	ERR_FAIL_COND_V(mobile_persistent_notification_task == UIBackgroundTaskInvalid, ERR_UNAVAILABLE);
	mobile_persistent_notification_active = true;
	if (!is_app_focused()) {
		start_mobile_background_processing();
	}
	return OK;
}

Error OS_IOS::update_mobile_persistent_notification(const String &p_title, const String &p_message) {
	ERR_FAIL_COND_V(!mobile_persistent_notification_active, ERR_UNCONFIGURED);
	mobile_persistent_notification_title = p_title;
	mobile_persistent_notification_message = p_message;
	return OK;
}

void OS_IOS::hide_mobile_persistent_notification() {
	stop_mobile_background_processing();

	mobile_persistent_notification_active = false;
	mobile_persistent_notification_title.clear();
	mobile_persistent_notification_message.clear();

	if (mobile_persistent_notification_task != UIBackgroundTaskInvalid) {
		UIBackgroundTaskIdentifier task = mobile_persistent_notification_task;
		mobile_persistent_notification_task = UIBackgroundTaskInvalid;
		[[UIApplication sharedApplication] endBackgroundTask:task];
	}
}

#endif // IOS_ENABLED
