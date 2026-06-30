/**************************************************************************/
/*  MobilePersistentNotificationService.kt                                */
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

package org.godotengine.godot.service

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.os.Build
import android.os.IBinder
import android.util.Log
import org.godotengine.godot.R
import java.util.concurrent.atomic.AtomicBoolean

/**
 * Foreground service used to keep Godot's main loop running while the app is backgrounded.
 */
class MobilePersistentNotificationService : Service() {

	companion object {
		private val TAG = MobilePersistentNotificationService::class.java.simpleName

		private const val ACTION_SHOW = "org.godotengine.godot.action.SHOW_MOBILE_PERSISTENT_NOTIFICATION"
		private const val ACTION_STOP = "org.godotengine.godot.action.STOP_MOBILE_PERSISTENT_NOTIFICATION"
		private const val EXTRA_TITLE = "title"
		private const val EXTRA_MESSAGE = "message"
		private const val CHANNEL_ID = "godot_mobile_persistent_notification"
		private const val NOTIFICATION_ID = 14001

		private val active = AtomicBoolean(false)

		@JvmStatic
		fun isActive() = active.get()

		@JvmStatic
		fun show(context: Context, title: String, message: String): Boolean {
			val serviceIntent = Intent(context, MobilePersistentNotificationService::class.java).apply {
				action = ACTION_SHOW
				putExtra(EXTRA_TITLE, title)
				putExtra(EXTRA_MESSAGE, message)
			}

			return try {
				active.set(true)
				if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
					context.startForegroundService(serviceIntent)
				} else {
					context.startService(serviceIntent)
				}
				true
			} catch (e: Exception) {
				active.set(false)
				Log.w(TAG, "Unable to start persistent notification service", e)
				false
			}
		}

		@JvmStatic
		fun update(context: Context, title: String, message: String): Boolean {
			if (!active.get()) {
				return false
			}
			return show(context, title, message)
		}

		@JvmStatic
		fun stop(context: Context) {
			active.set(false)
			val serviceIntent = Intent(context, MobilePersistentNotificationService::class.java).apply {
				action = ACTION_STOP
			}

			try {
				context.startService(serviceIntent)
			} catch (e: Exception) {
				Log.v(TAG, "Unable to send stop request to persistent notification service", e)
				context.stopService(serviceIntent)
			}
		}
	}

	override fun onBind(intent: Intent?): IBinder? = null

	override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
		if (intent?.action == ACTION_STOP) {
			stopForeground(Service.STOP_FOREGROUND_REMOVE)
			stopSelf(startId)
			return START_NOT_STICKY
		}

		val title = intent?.getStringExtra(EXTRA_TITLE).orEmpty()
		val message = intent?.getStringExtra(EXTRA_MESSAGE).orEmpty()
		val notification = buildNotification(title, message)

		try {
			if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
				startForeground(NOTIFICATION_ID, notification, ServiceInfo.FOREGROUND_SERVICE_TYPE_DATA_SYNC)
			} else {
				startForeground(NOTIFICATION_ID, notification)
			}
			active.set(true)
		} catch (e: Exception) {
			active.set(false)
			Log.w(TAG, "Unable to promote persistent notification service to foreground", e)
			stopSelf(startId)
		}

		return START_NOT_STICKY
	}

	override fun onDestroy() {
		active.set(false)
		super.onDestroy()
	}

	private fun buildNotification(title: String, message: String): Notification {
		val launchIntent = packageManager.getLaunchIntentForPackage(packageName)
		val contentIntent = launchIntent?.let {
			PendingIntent.getActivity(
				this,
				0,
				it,
				PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE
			)
		}

		if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
			val channel = NotificationChannel(
				CHANNEL_ID,
				getString(R.string.mobile_persistent_notification_channel_name),
				NotificationManager.IMPORTANCE_LOW
			).apply {
				setShowBadge(false)
			}
			getSystemService(NotificationManager::class.java).createNotificationChannel(channel)
		}

		val notificationTitle = title.ifBlank { getString(R.string.godot_project_name_string) }
		val builder = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
			Notification.Builder(this, CHANNEL_ID)
		} else {
			@Suppress("DEPRECATION")
			Notification.Builder(this)
		}

		val notificationBuilder = builder
			.setSmallIcon(R.mipmap.icon_monochrome)
			.setContentTitle(notificationTitle)
			.setContentText(message)
			.setOngoing(true)
			.setOnlyAlertOnce(true)
			.setShowWhen(false)
			.setCategory(Notification.CATEGORY_SERVICE)

		if (contentIntent != null) {
			notificationBuilder.setContentIntent(contentIntent)
		}

		return notificationBuilder.build()
	}
}
