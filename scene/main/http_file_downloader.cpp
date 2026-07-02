/**************************************************************************/
/*  http_file_downloader.cpp                                              */
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

#include "http_file_downloader.h"

#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/object/class_db.h"
#include "core/os/os.h"
#include "core/variant/array.h"

void HTTPFileDownloader::_coordinator_thread_func(void *p_userdata) {
	HTTPFileDownloader *downloader = static_cast<HTTPFileDownloader *>(p_userdata);
	downloader->_coordinator_loop();
	downloader->thread_done.set();
}

void HTTPFileDownloader::_segment_thread_func(void *p_userdata) {
	SegmentDownload *segment = static_cast<SegmentDownload *>(p_userdata);

	ResponseInfo response;
	int64_t written = 0;
	segment->result = segment->downloader->_perform_request(segment->url, HTTPClient::METHOD_GET, segment->headers, segment->range_start, segment->range_end, false, segment->file, segment->write_offset, segment->item_index, true, &response, &written);
	segment->response_code = response.response_code;
}

Error HTTPFileDownloader::_start_downloads(const Vector<DownloadItem> &p_items) {
#if !defined(THREADS_ENABLED) || defined(WEB_ENABLED)
	ERR_FAIL_V_MSG(ERR_UNAVAILABLE, "HTTPFileDownloader requires threaded HTTPClient support. The Web platform does not support the blocking threaded HTTPClient mode used for high-performance downloads.");
#else
	ERR_FAIL_COND_V(!is_inside_tree(), ERR_UNCONFIGURED);
	ERR_FAIL_COND_V_MSG(running.is_set(), ERR_BUSY, "HTTPFileDownloader is already downloading. Wait for completion or cancel it before starting another batch.");
	ERR_FAIL_COND_V(p_items.is_empty(), ERR_INVALID_PARAMETER);

	{
		MutexLock lock(state_mutex);
		items = p_items;
		active_download_index = -1;
		batch_result = RESULT_SUCCESS;
		batch_started_usec = OS::get_singleton()->get_ticks_usec();
		batch_finished_usec = 0;
		last_progress_signal_usec = 0;
		completion_emitted = false;
	}

	cancel_requested.clear();
	thread_done.clear();
	finished.clear();
	running.set();
	set_process_internal(true);
	coordinator_thread.start(_coordinator_thread_func, this);

	return OK;
#endif
}

void HTTPFileDownloader::_coordinator_loop() {
	Result final_result = RESULT_SUCCESS;
	int cancel_from_index = -1;

	for (int i = 0; i < items.size(); i++) {
		if (cancel_requested.is_set()) {
			_set_item_status(i, DOWNLOAD_STATUS_CANCELED, RESULT_CANCELED);
			final_result = RESULT_CANCELED;
			cancel_from_index = i + 1;
			break;
		}

		Result item_result = _download_item(i);
		if (item_result != RESULT_SUCCESS && final_result == RESULT_SUCCESS) {
			final_result = item_result;
		}

		if (cancel_requested.is_set()) {
			final_result = RESULT_CANCELED;
			cancel_from_index = i + 1;
			break;
		}

		if (item_result != RESULT_SUCCESS && stop_on_error) {
			cancel_from_index = i + 1;
			break;
		}
	}

	if (cancel_requested.is_set()) {
		final_result = RESULT_CANCELED;
	}
	if (cancel_from_index >= 0) {
		for (int i = cancel_from_index; i < items.size(); i++) {
			_set_item_status(i, DOWNLOAD_STATUS_CANCELED, RESULT_CANCELED);
		}
	}

	_set_active_download(-1);
	_set_batch_finished(final_result);
}

HTTPFileDownloader::Result HTTPFileDownloader::_download_item(int p_index) {
	_set_active_download(p_index);
	_set_item_status(p_index, DOWNLOAD_STATUS_PREPARING);
	_reset_item_downloaded_bytes(p_index);

	ResponseInfo probe;
	Result result = _probe_item(p_index, &probe);
	if (cancel_requested.is_set()) {
		String path;
		{
			MutexLock lock(state_mutex);
			if (p_index >= 0 && p_index < items.size()) {
				path = items[p_index].path;
			}
		}
		_set_item_status(p_index, DOWNLOAD_STATUS_CANCELED, RESULT_CANCELED, probe.response_code);
		if (!keep_partial_files && !path.is_empty()) {
			DirAccess::remove_absolute(path);
		}
		return RESULT_CANCELED;
	}

	if (result != RESULT_SUCCESS) {
		String path;
		{
			MutexLock lock(state_mutex);
			if (p_index >= 0 && p_index < items.size()) {
				path = items[p_index].path;
			}
		}
		_set_item_status(p_index, DOWNLOAD_STATUS_FAILED, result, probe.response_code);
		if (!keep_partial_files && !path.is_empty()) {
			DirAccess::remove_absolute(path);
		}
		return result;
	}

	String original_url;
	{
		MutexLock lock(state_mutex);
		if (p_index >= 0 && p_index < items.size()) {
			original_url = items[p_index].url;
		}
	}
	const String download_url = probe.final_url.is_empty() ? original_url : probe.final_url;
	const int effective_thread_count = _get_effective_thread_count(probe.total_bytes);
	_set_item_thread_count(p_index, probe.range_supported ? effective_thread_count : 1);
	_set_item_status(p_index, DOWNLOAD_STATUS_DOWNLOADING, RESULT_SUCCESS, probe.response_code);

	if (probe.range_supported && effective_thread_count > 1) {
		result = _download_item_parallel(p_index, download_url, effective_thread_count);
		if (result == RESULT_RANGE_NOT_SUPPORTED && !cancel_requested.is_set()) {
			_reset_item_downloaded_bytes(p_index);
			result = _download_item_single(p_index, download_url);
		}
	} else {
		result = _download_item_single(p_index, download_url);
	}

	if (cancel_requested.is_set()) {
		result = RESULT_CANCELED;
	}

	if (result == RESULT_SUCCESS) {
		_set_item_status(p_index, DOWNLOAD_STATUS_COMPLETED, RESULT_SUCCESS);
	} else if (result == RESULT_CANCELED) {
		String path;
		{
			MutexLock lock(state_mutex);
			if (p_index >= 0 && p_index < items.size()) {
				path = items[p_index].path;
			}
		}
		_set_item_status(p_index, DOWNLOAD_STATUS_CANCELED, RESULT_CANCELED);
		if (!keep_partial_files && !path.is_empty()) {
			DirAccess::remove_absolute(path);
		}
	} else {
		String path;
		{
			MutexLock lock(state_mutex);
			if (p_index >= 0 && p_index < items.size()) {
				path = items[p_index].path;
			}
		}
		_set_item_status(p_index, DOWNLOAD_STATUS_FAILED, result);
		if (!keep_partial_files && !path.is_empty()) {
			DirAccess::remove_absolute(path);
		}
	}

	return result;
}

HTTPFileDownloader::Result HTTPFileDownloader::_download_item_single(int p_index, const String &p_url) {
	String path;
	Vector<String> headers;
	{
		MutexLock lock(state_mutex);
		ERR_FAIL_INDEX_V(p_index, items.size(), RESULT_INVALID_URL);
		path = items[p_index].path;
		headers = items[p_index].headers;
		items.write[p_index].assigned_thread_count = 1;
	}

	Result open_result = RESULT_SUCCESS;
	Ref<FileAccess> file = _open_output_file(path, false, -1, &open_result);
	if (file.is_null()) {
		return open_result;
	}

	ResponseInfo response;
	int64_t written = 0;
	Result result = _perform_request(p_url, HTTPClient::METHOD_GET, headers, -1, -1, false, file, 0, p_index, false, &response, &written);
	file->flush();
	return result;
}

HTTPFileDownloader::Result HTTPFileDownloader::_download_item_parallel(int p_index, const String &p_url, int p_thread_count) {
	String path;
	Vector<String> headers;
	int64_t total_bytes = -1;
	{
		MutexLock lock(state_mutex);
		ERR_FAIL_INDEX_V(p_index, items.size(), RESULT_INVALID_URL);
		path = items[p_index].path;
		headers = items[p_index].headers;
		total_bytes = items[p_index].total_bytes;
	}

	if (total_bytes <= 0) {
		return _download_item_single(p_index, p_url);
	}

	const int segment_count = (int)MIN((int64_t)p_thread_count, total_bytes);
	if (segment_count <= 1) {
		return _download_item_single(p_index, p_url);
	}

	Result open_result = RESULT_SUCCESS;
	Ref<FileAccess> file = _open_output_file(path, true, total_bytes, &open_result);
	if (file.is_null()) {
		return open_result;
	}

	Vector<SegmentDownload *> segments;
	Vector<Thread *> threads;
	segments.resize(segment_count);
	threads.resize(segment_count);

	int64_t segment_size = total_bytes / segment_count;
	int64_t remainder = total_bytes % segment_count;
	int64_t range_start = 0;

	for (int i = 0; i < segment_count; i++) {
		const int64_t size = segment_size + (i < remainder ? 1 : 0);
		SegmentDownload *segment = memnew(SegmentDownload);
		segment->downloader = this;
		segment->item_index = p_index;
		segment->url = p_url;
		segment->headers = headers;
		segment->file = file;
		segment->range_start = range_start;
		segment->range_end = range_start + size - 1;
		segment->write_offset = range_start;
		segments.write[i] = segment;

		Thread *thread = memnew(Thread);
		threads.write[i] = thread;
#ifdef THREADS_ENABLED
		Thread::ID thread_id = thread->start(_segment_thread_func, segment);
		if (thread_id == Thread::UNASSIGNED_ID) {
			segment->result = RESULT_UNAVAILABLE;
		}
#else
		segment->result = RESULT_UNAVAILABLE;
#endif
		range_start += size;
	}

	Result final_result = RESULT_SUCCESS;
	int final_response_code = 0;
	for (int i = 0; i < segment_count; i++) {
		Thread *thread = threads[i];
		if (thread != nullptr && thread->is_started()) {
			thread->wait_to_finish();
		}

		SegmentDownload *segment = segments[i];
		if (segment != nullptr) {
			if (segment->result != RESULT_SUCCESS && final_result == RESULT_SUCCESS) {
				final_result = segment->result;
				final_response_code = segment->response_code;
			}
			memdelete(segment);
		}

		if (thread != nullptr) {
			memdelete(thread);
		}
	}

	file->flush();

	if (final_response_code != 0) {
		_set_item_status(p_index, final_result == RESULT_SUCCESS ? DOWNLOAD_STATUS_DOWNLOADING : DOWNLOAD_STATUS_FAILED, final_result, final_response_code);
	}

	return final_result;
}

HTTPFileDownloader::Result HTTPFileDownloader::_probe_item(int p_index, ResponseInfo *r_response) {
	String url;
	Vector<String> headers;
	{
		MutexLock lock(state_mutex);
		ERR_FAIL_INDEX_V(p_index, items.size(), RESULT_INVALID_URL);
		url = items[p_index].url;
		headers = items[p_index].headers;
	}

	Result result = _perform_request(url, HTTPClient::METHOD_GET, headers, 0, 0, true, Ref<FileAccess>(), 0, p_index, false, r_response, nullptr);
	if (result != RESULT_SUCCESS) {
		return result;
	}

	if (r_response->response_code == HTTPClient::RESPONSE_PARTIAL_CONTENT) {
		r_response->range_supported = r_response->total_bytes >= 0;
	} else {
		r_response->range_supported = false;
	}

	_set_item_response(p_index, *r_response);
	return RESULT_SUCCESS;
}

HTTPFileDownloader::Result HTTPFileDownloader::_perform_request(const String &p_url, HTTPClient::Method p_method, const Vector<String> &p_headers, int64_t p_range_start, int64_t p_range_end, bool p_headers_only, const Ref<FileAccess> &p_output_file, int64_t p_write_offset, int p_item_index, bool p_require_partial_response, ResponseInfo *r_response, int64_t *r_written) {
	String current_url = p_url;
	int redirect_count = 0;

	while (true) {
		if (cancel_requested.is_set()) {
			return RESULT_CANCELED;
		}

		ParsedURL parsed_url;
		Error parse_err = _parse_url(current_url, &parsed_url);
		if (parse_err != OK) {
			return RESULT_INVALID_URL;
		}

		Ref<HTTPClient> client = Ref<HTTPClient>(HTTPClient::create());
		if (client.is_null()) {
			return RESULT_UNAVAILABLE;
		}

		client->set_blocking_mode(true);
		client->set_read_chunk_size(download_chunk_size);
		client->set_http_proxy(http_proxy_host, http_proxy_port);
		client->set_https_proxy(https_proxy_host, https_proxy_port);

		Error err = client->connect_to_host(parsed_url.host, parsed_url.port, parsed_url.use_tls ? tls_options : Ref<TLSOptions>());
		if (err != OK) {
			return RESULT_CANT_CONNECT;
		}

		bool connected = false;
		while (!cancel_requested.is_set() && !connected) {
			switch (client->get_status()) {
				case HTTPClient::STATUS_RESOLVING:
				case HTTPClient::STATUS_CONNECTING: {
					client->poll();
					OS::get_singleton()->delay_usec(1000);
				} break;
				case HTTPClient::STATUS_CONNECTED: {
					connected = true;
				} break;
				case HTTPClient::STATUS_CANT_RESOLVE: {
					return RESULT_CANT_RESOLVE;
				} break;
				case HTTPClient::STATUS_CANT_CONNECT: {
					return RESULT_CANT_CONNECT;
				} break;
				case HTTPClient::STATUS_TLS_HANDSHAKE_ERROR: {
					return RESULT_TLS_HANDSHAKE_ERROR;
				} break;
				case HTTPClient::STATUS_CONNECTION_ERROR: {
					return RESULT_CONNECTION_ERROR;
				} break;
				default: {
					client->poll();
				} break;
			}
		}

		if (cancel_requested.is_set()) {
			client->close();
			return RESULT_CANCELED;
		}

		Vector<String> request_headers = _make_request_headers(p_headers, p_range_start, p_range_end);
		err = client->request(p_method, parsed_url.request, request_headers, nullptr, 0);
		if (err != OK) {
			client->close();
			return RESULT_CONNECTION_ERROR;
		}

		bool got_response = false;
		while (!cancel_requested.is_set() && !got_response) {
			switch (client->get_status()) {
				case HTTPClient::STATUS_REQUESTING: {
					client->poll();
					OS::get_singleton()->delay_usec(1000);
				} break;
				case HTTPClient::STATUS_BODY:
				case HTTPClient::STATUS_CONNECTED: {
					got_response = true;
				} break;
				case HTTPClient::STATUS_CANT_RESOLVE: {
					client->close();
					return RESULT_CANT_RESOLVE;
				} break;
				case HTTPClient::STATUS_CANT_CONNECT: {
					client->close();
					return RESULT_CANT_CONNECT;
				} break;
				case HTTPClient::STATUS_TLS_HANDSHAKE_ERROR: {
					client->close();
					return RESULT_TLS_HANDSHAKE_ERROR;
				} break;
				case HTTPClient::STATUS_CONNECTION_ERROR:
				case HTTPClient::STATUS_DISCONNECTED: {
					client->close();
					return RESULT_CONNECTION_ERROR;
				} break;
				default: {
					client->poll();
				} break;
			}
		}

		if (cancel_requested.is_set()) {
			client->close();
			return RESULT_CANCELED;
		}

		if (!client->has_response()) {
			client->close();
			return RESULT_NO_RESPONSE;
		}

		ResponseInfo response;
		response.response_code = client->get_response_code();
		List<String> response_headers;
		client->get_response_headers(&response_headers);
		for (const String &E : response_headers) {
			response.headers.push_back(E);
		}
		response.total_bytes = _get_total_bytes_from_headers(response.response_code, response.headers);
		response.range_supported = response.response_code == HTTPClient::RESPONSE_PARTIAL_CONTENT && response.total_bytes >= 0;
		response.final_url = current_url;

		if (_is_redirect_response(response.response_code)) {
			if (redirect_count >= max_redirects) {
				if (r_response != nullptr) {
					*r_response = response;
				}
				client->close();
				return RESULT_REDIRECT_LIMIT_REACHED;
			}

			const String location = _get_header_value(response.headers, "Location");
			if (location.is_empty()) {
				if (r_response != nullptr) {
					*r_response = response;
				}
				client->close();
				return RESULT_HTTP_ERROR;
			}

			current_url = _resolve_redirect_url(parsed_url, location);
			redirect_count++;
			client->close();
			continue;
		}

		if (r_response != nullptr) {
			*r_response = response;
		}
		_set_item_response(p_item_index, response);

		if (p_require_partial_response && response.response_code != HTTPClient::RESPONSE_PARTIAL_CONTENT) {
			client->close();
			return _is_success_response(response.response_code) ? RESULT_RANGE_NOT_SUPPORTED : RESULT_HTTP_ERROR;
		}
		if (!_is_success_response(response.response_code)) {
			client->close();
			return RESULT_HTTP_ERROR;
		}

		if (p_headers_only) {
			client->close();
			return RESULT_SUCCESS;
		}

		int64_t written = 0;
		int64_t write_offset = p_write_offset;
		const int64_t body_length = client->get_response_body_length();
		if (response.total_bytes >= 0 && p_range_start < 0) {
			_set_item_total_bytes(p_item_index, response.total_bytes);
		}

		if (client->get_status() == HTTPClient::STATUS_CONNECTED || body_length == 0) {
			client->close();
			if (r_written != nullptr) {
				*r_written = 0;
			}
			return RESULT_SUCCESS;
		}

		while (!cancel_requested.is_set()) {
			Error poll_err = client->poll();
			if (poll_err != OK && client->get_status() != HTTPClient::STATUS_DISCONNECTED) {
				client->close();
				return RESULT_CONNECTION_ERROR;
			}

			if (client->get_status() == HTTPClient::STATUS_BODY) {
				PackedByteArray chunk = client->read_response_body_chunk();
				if (!chunk.is_empty()) {
					if (p_output_file.is_valid()) {
						MutexLock lock(file_mutex);
						p_output_file->seek(write_offset);
						p_output_file->store_buffer(chunk.ptr(), chunk.size());
						if (p_output_file->get_error() != OK) {
							client->close();
							return RESULT_DOWNLOAD_FILE_WRITE_ERROR;
						}
					}

					write_offset += chunk.size();
					written += chunk.size();
					_add_downloaded_bytes(p_item_index, chunk.size());
				}
			}

			const HTTPClient::Status status = client->get_status();
			if (status == HTTPClient::STATUS_CONNECTED || status == HTTPClient::STATUS_DISCONNECTED) {
				break;
			}
			if (status == HTTPClient::STATUS_CONNECTION_ERROR) {
				client->close();
				return RESULT_CONNECTION_ERROR;
			}
			if (status == HTTPClient::STATUS_TLS_HANDSHAKE_ERROR) {
				client->close();
				return RESULT_TLS_HANDSHAKE_ERROR;
			}

			if (body_length >= 0 && written >= body_length) {
				break;
			}
		}

		client->close();

		if (cancel_requested.is_set()) {
			return RESULT_CANCELED;
		}

		if (r_written != nullptr) {
			*r_written = written;
		}
		return RESULT_SUCCESS;
	}
}

Error HTTPFileDownloader::_parse_url(const String &p_url, ParsedURL *r_url) const {
	ERR_FAIL_NULL_V(r_url, ERR_INVALID_PARAMETER);

	String scheme;
	String fragment;
	Error err = p_url.parse_url(scheme, r_url->host, r_url->port, r_url->request, fragment);
	ERR_FAIL_COND_V_MSG(err != OK, err, vformat("Error parsing URL: '%s'.", p_url));

	if (scheme == "https://") {
		r_url->use_tls = true;
	} else if (scheme == "http://") {
		r_url->use_tls = false;
	} else {
		ERR_FAIL_V_MSG(ERR_INVALID_PARAMETER, vformat("Invalid URL scheme: '%s'.", scheme));
	}

	if (r_url->port == 0) {
		r_url->port = r_url->use_tls ? 443 : 80;
	}
	if (r_url->request.is_empty()) {
		r_url->request = "/";
	}
	r_url->source_url = p_url;
	return OK;
}

Vector<String> HTTPFileDownloader::_make_request_headers(const Vector<String> &p_headers, int64_t p_range_start, int64_t p_range_end) const {
	Vector<String> request_headers;
	bool has_accept_encoding = false;

	for (const String &header : p_headers) {
		const String lower = header.strip_edges().to_lower();
		const int separator = lower.find_char(':');
		const String header_name = separator > 0 ? lower.substr(0, separator).strip_edges() : String();
		if (p_range_start >= 0 && header_name == "range") {
			continue;
		}
		if (header_name == "accept-encoding") {
			has_accept_encoding = true;
		}
		request_headers.push_back(header);
	}

	if (!has_accept_encoding) {
		request_headers.push_back("Accept-Encoding: identity");
	}

	if (p_range_start >= 0) {
		if (p_range_end >= p_range_start) {
			request_headers.push_back(vformat("Range: bytes=%d-%d", p_range_start, p_range_end));
		} else {
			request_headers.push_back(vformat("Range: bytes=%d-", p_range_start));
		}
	}

	return request_headers;
}

Error HTTPFileDownloader::_validate_headers(const Vector<String> &p_headers) const {
	for (int i = 0; i < p_headers.size(); i++) {
		String sanitized = p_headers[i].strip_edges();
		ERR_FAIL_COND_V_MSG(sanitized.is_empty(), ERR_INVALID_PARAMETER, vformat("Invalid HTTP header at index %d: empty.", i));
		ERR_FAIL_COND_V_MSG(sanitized.find_char(':') < 1, ERR_INVALID_PARAMETER, vformat("Invalid HTTP header at index %d: String must contain header-value pair, delimited by ':', but was: '%s'.", i, p_headers[i]));
	}

	return OK;
}

Error HTTPFileDownloader::_append_variant_headers(const Variant &p_headers, Vector<String> *r_headers) const {
	ERR_FAIL_NULL_V(r_headers, ERR_INVALID_PARAMETER);

	if (p_headers.get_type() == Variant::NIL) {
		return OK;
	}

	if (p_headers.get_type() == Variant::PACKED_STRING_ARRAY) {
		PackedStringArray headers = p_headers;
		for (int i = 0; i < headers.size(); i++) {
			r_headers->push_back(headers[i]);
		}
		return OK;
	}

	if (p_headers.get_type() == Variant::ARRAY) {
		Array headers = p_headers;
		for (int i = 0; i < headers.size(); i++) {
			r_headers->push_back(String(headers[i]));
		}
		return OK;
	}

	return ERR_INVALID_PARAMETER;
}

String HTTPFileDownloader::_get_header_value(const Vector<String> &p_headers, const String &p_header_name) const {
	const String lower_name = p_header_name.to_lower();
	for (const String &header : p_headers) {
		const int separator = header.find_char(':');
		if (separator > 0 && header.substr(0, separator).strip_edges().to_lower() == lower_name) {
			return header.substr(separator + 1).strip_edges();
		}
	}
	return String();
}

bool HTTPFileDownloader::_is_redirect_response(int p_response_code) const {
	return p_response_code == HTTPClient::RESPONSE_MOVED_PERMANENTLY ||
			p_response_code == HTTPClient::RESPONSE_FOUND ||
			p_response_code == HTTPClient::RESPONSE_SEE_OTHER ||
			p_response_code == HTTPClient::RESPONSE_TEMPORARY_REDIRECT ||
			p_response_code == HTTPClient::RESPONSE_PERMANENT_REDIRECT ||
			p_response_code == HTTPClient::RESPONSE_USE_PROXY;
}

bool HTTPFileDownloader::_is_success_response(int p_response_code) const {
	return p_response_code >= 200 && p_response_code < 300;
}

String HTTPFileDownloader::_resolve_redirect_url(const ParsedURL &p_base_url, const String &p_location) const {
	if (p_location.begins_with("http://") || p_location.begins_with("https://")) {
		return p_location;
	}

	String prefix = p_base_url.use_tls ? "https://" : "http://";
	prefix += p_base_url.host;
	if ((p_base_url.use_tls && p_base_url.port != 443) || (!p_base_url.use_tls && p_base_url.port != 80)) {
		prefix += ":" + itos(p_base_url.port);
	}

	if (p_location.begins_with("/")) {
		return prefix + p_location;
	}

	String base_dir = p_base_url.request.get_base_dir();
	if (!base_dir.begins_with("/")) {
		base_dir = "/" + base_dir;
	}
	if (!base_dir.ends_with("/")) {
		base_dir += "/";
	}
	return prefix + base_dir + p_location;
}

int64_t HTTPFileDownloader::_get_total_bytes_from_headers(int p_response_code, const Vector<String> &p_headers) const {
	if (p_response_code == HTTPClient::RESPONSE_PARTIAL_CONTENT) {
		const String content_range = _get_header_value(p_headers, "Content-Range");
		const int slash = content_range.find_char('/');
		if (slash >= 0) {
			const String total = content_range.substr(slash + 1).strip_edges();
			if (!total.is_empty() && total != "*") {
				return total.to_int();
			}
		}
	}

	const String content_length = _get_header_value(p_headers, "Content-Length");
	if (!content_length.is_empty()) {
		return content_length.to_int();
	}

	return -1;
}

Ref<FileAccess> HTTPFileDownloader::_open_output_file(const String &p_path, bool p_read_write, int64_t p_size, Result *r_result) const {
	if (r_result != nullptr) {
		*r_result = RESULT_SUCCESS;
	}

	const String base_dir = p_path.get_base_dir();
	if (!base_dir.is_empty() && base_dir != ".") {
		Error dir_err = DirAccess::make_dir_recursive_absolute(base_dir);
		if (dir_err != OK) {
			if (r_result != nullptr) {
				*r_result = RESULT_DOWNLOAD_FILE_CANT_OPEN;
			}
			return Ref<FileAccess>();
		}
	}

	Error open_err = OK;
	Ref<FileAccess> file = FileAccess::open(p_path, p_read_write ? FileAccess::WRITE_READ : FileAccess::WRITE, &open_err);
	if (file.is_null() || open_err != OK) {
		if (r_result != nullptr) {
			*r_result = RESULT_DOWNLOAD_FILE_CANT_OPEN;
		}
		return Ref<FileAccess>();
	}

	if (p_size >= 0) {
		file->resize(p_size);
	}

	return file;
}

int HTTPFileDownloader::_get_effective_thread_count(int64_t p_total_bytes) const {
	const int max_threads = CLAMP(max_thread_count, 1, MAX_THREAD_LIMIT);
	if (thread_count > 0) {
		return CLAMP(thread_count, 1, max_threads);
	}

	if (min_parallel_size > 0 && p_total_bytes < min_parallel_size) {
		return 1;
	}

	int processor_count = 1;
	if (OS::get_singleton() != nullptr) {
		processor_count = MAX(OS::get_singleton()->get_processor_count(), 1);
	}

	const int64_t size_step = MAX(min_parallel_size, (int64_t)1);
	const int size_limited_threads = (int)CLAMP(p_total_bytes / size_step + 1, (int64_t)1, (int64_t)max_threads);
	return CLAMP(MIN(processor_count, size_limited_threads), 1, max_threads);
}

void HTTPFileDownloader::_set_active_download(int p_index) {
	MutexLock lock(state_mutex);
	active_download_index = p_index;
}

void HTTPFileDownloader::_set_item_status(int p_index, DownloadStatus p_status, Result p_result, int p_response_code) {
	MutexLock lock(state_mutex);
	if (p_index < 0 || p_index >= items.size()) {
		return;
	}

	DownloadItem &item = items.write[p_index];
	item.status = p_status;
	item.result = p_result;
	if (p_response_code != 0) {
		item.response_code = p_response_code;
	}
	const uint64_t now = OS::get_singleton()->get_ticks_usec();
	if ((p_status == DOWNLOAD_STATUS_PREPARING || p_status == DOWNLOAD_STATUS_DOWNLOADING) && item.started_usec == 0) {
		item.started_usec = now;
		item.finished_usec = 0;
	}
	if (p_status == DOWNLOAD_STATUS_COMPLETED || p_status == DOWNLOAD_STATUS_FAILED || p_status == DOWNLOAD_STATUS_CANCELED) {
		item.finished_usec = now;
	}
}

void HTTPFileDownloader::_set_item_response(int p_index, const ResponseInfo &p_response) {
	MutexLock lock(state_mutex);
	if (p_index < 0 || p_index >= items.size()) {
		return;
	}

	DownloadItem &item = items.write[p_index];
	item.response_code = p_response.response_code;
	item.response_headers = p_response.headers;
	if (p_response.total_bytes >= 0) {
		item.total_bytes = p_response.total_bytes;
	}
}

void HTTPFileDownloader::_set_item_total_bytes(int p_index, int64_t p_total_bytes) {
	if (p_total_bytes < 0) {
		return;
	}

	MutexLock lock(state_mutex);
	if (p_index < 0 || p_index >= items.size()) {
		return;
	}
	items.write[p_index].total_bytes = p_total_bytes;
}

void HTTPFileDownloader::_set_item_thread_count(int p_index, int p_thread_count) {
	MutexLock lock(state_mutex);
	if (p_index < 0 || p_index >= items.size()) {
		return;
	}
	items.write[p_index].assigned_thread_count = p_thread_count;
}

void HTTPFileDownloader::_reset_item_downloaded_bytes(int p_index) {
	MutexLock lock(state_mutex);
	if (p_index < 0 || p_index >= items.size()) {
		return;
	}
	items.write[p_index].downloaded_bytes = 0;
}

void HTTPFileDownloader::_add_downloaded_bytes(int p_index, int64_t p_bytes) {
	if (p_bytes <= 0) {
		return;
	}

	MutexLock lock(state_mutex);
	if (p_index < 0 || p_index >= items.size()) {
		return;
	}
	items.write[p_index].downloaded_bytes += p_bytes;
}

void HTTPFileDownloader::_set_batch_finished(Result p_result) {
	MutexLock lock(state_mutex);
	batch_result = p_result;
	batch_finished_usec = OS::get_singleton()->get_ticks_usec();
}

Dictionary HTTPFileDownloader::_get_item_status_unlocked(const DownloadItem &p_item, uint64_t p_now_usec) const {
	Dictionary status;
	status["id"] = p_item.id;
	status["url"] = p_item.url;
	status["path"] = p_item.path;
	status["status"] = p_item.status;
	status["result"] = p_item.result;
	status["response_code"] = p_item.response_code;
	status["downloaded_bytes"] = p_item.downloaded_bytes;
	status["total_bytes"] = p_item.total_bytes;
	status["thread_count"] = p_item.assigned_thread_count;

	const uint64_t end_usec = p_item.finished_usec != 0 ? p_item.finished_usec : p_now_usec;
	const double elapsed = p_item.started_usec == 0 ? 0.0 : (double)(end_usec - p_item.started_usec) / 1000000.0;
	const double speed = elapsed > 0.0 ? (double)p_item.downloaded_bytes / elapsed : 0.0;
	double progress = 0.0;
	double eta = -1.0;
	if (p_item.total_bytes > 0) {
		progress = MIN((double)p_item.downloaded_bytes / (double)p_item.total_bytes, 1.0);
		if (speed > 0.0 && p_item.downloaded_bytes < p_item.total_bytes) {
			eta = (double)(p_item.total_bytes - p_item.downloaded_bytes) / speed;
		} else if (p_item.downloaded_bytes >= p_item.total_bytes) {
			eta = 0.0;
		}
	} else if (p_item.status == DOWNLOAD_STATUS_COMPLETED) {
		progress = 1.0;
		eta = 0.0;
	}

	status["progress"] = progress;
	status["elapsed_time"] = elapsed;
	status["estimated_remaining_time"] = eta;
	status["bytes_per_second"] = speed;

	PackedStringArray headers;
	for (const String &header : p_item.response_headers) {
		headers.push_back(header);
	}
	status["headers"] = headers;
	return status;
}

int64_t HTTPFileDownloader::_get_downloaded_bytes_unlocked() const {
	int64_t downloaded = 0;
	for (const DownloadItem &item : items) {
		downloaded += item.downloaded_bytes;
	}
	return downloaded;
}

int64_t HTTPFileDownloader::_get_total_bytes_unlocked(bool *r_has_unknown) const {
	int64_t total = 0;
	bool has_unknown = false;
	for (const DownloadItem &item : items) {
		if (item.total_bytes < 0) {
			has_unknown = true;
		} else {
			total += item.total_bytes;
		}
	}

	if (r_has_unknown != nullptr) {
		*r_has_unknown = has_unknown;
	}
	return has_unknown ? -1 : total;
}

double HTTPFileDownloader::_get_progress_unlocked(uint64_t p_now_usec) const {
	bool has_unknown = false;
	const int64_t total = _get_total_bytes_unlocked(&has_unknown);
	if (!has_unknown && total > 0) {
		return MIN((double)_get_downloaded_bytes_unlocked() / (double)total, 1.0);
	}

	if (items.is_empty()) {
		return 0.0;
	}

	double progress_units = 0.0;
	for (const DownloadItem &item : items) {
		if (item.status == DOWNLOAD_STATUS_COMPLETED || item.status == DOWNLOAD_STATUS_FAILED || item.status == DOWNLOAD_STATUS_CANCELED) {
			progress_units += 1.0;
		} else if ((item.status == DOWNLOAD_STATUS_DOWNLOADING || item.status == DOWNLOAD_STATUS_PREPARING) && item.total_bytes > 0) {
			progress_units += MIN((double)item.downloaded_bytes / (double)item.total_bytes, 1.0);
		}
	}

	return CLAMP(progress_units / (double)items.size(), 0.0, 1.0);
}

double HTTPFileDownloader::_get_elapsed_time_unlocked(uint64_t p_now_usec) const {
	if (batch_started_usec == 0) {
		return 0.0;
	}

	const uint64_t end_usec = batch_finished_usec != 0 ? batch_finished_usec : p_now_usec;
	return (double)(end_usec - batch_started_usec) / 1000000.0;
}

double HTTPFileDownloader::_get_bytes_per_second_unlocked(uint64_t p_now_usec) const {
	const double elapsed = _get_elapsed_time_unlocked(p_now_usec);
	return elapsed > 0.0 ? (double)_get_downloaded_bytes_unlocked() / elapsed : 0.0;
}

double HTTPFileDownloader::_get_estimated_remaining_time_unlocked(uint64_t p_now_usec) const {
	bool has_unknown = false;
	const int64_t total = _get_total_bytes_unlocked(&has_unknown);
	if (has_unknown || total < 0) {
		return -1.0;
	}

	const double speed = _get_bytes_per_second_unlocked(p_now_usec);
	if (speed <= 0.0) {
		return total == 0 ? 0.0 : -1.0;
	}

	const int64_t remaining = MAX((int64_t)0, total - _get_downloaded_bytes_unlocked());
	return (double)remaining / speed;
}

void HTTPFileDownloader::_emit_progress_signal() {
	const uint64_t now = OS::get_singleton()->get_ticks_usec();
	Dictionary active_status;
	int active_id = -1;
	String active_url;

	int64_t downloaded = 0;
	int64_t total = -1;
	double progress = 0.0;
	double elapsed = 0.0;
	double eta = -1.0;
	double speed = 0.0;

	{
		MutexLock lock(state_mutex);
		if (!running.is_set()) {
			return;
		}

		if (progress_update_interval > 0.0 && last_progress_signal_usec != 0 && (double)(now - last_progress_signal_usec) / 1000000.0 < progress_update_interval) {
			return;
		}
		last_progress_signal_usec = now;

		downloaded = _get_downloaded_bytes_unlocked();
		total = _get_total_bytes_unlocked();
		progress = _get_progress_unlocked(now);
		elapsed = _get_elapsed_time_unlocked(now);
		eta = _get_estimated_remaining_time_unlocked(now);
		speed = _get_bytes_per_second_unlocked(now);

		if (active_download_index >= 0 && active_download_index < items.size()) {
			active_status = _get_item_status_unlocked(items[active_download_index], now);
			active_id = items[active_download_index].id;
			active_url = items[active_download_index].url;
		}
	}

	emit_signal(SNAME("batch_progressed"), downloaded, total, progress, elapsed, eta, speed);

	if (active_id >= 0) {
		emit_signal(SNAME("download_progressed"), active_id, active_url, (int64_t)active_status["downloaded_bytes"], (int64_t)active_status["total_bytes"], (double)active_status["progress"], (double)active_status["elapsed_time"], (double)active_status["estimated_remaining_time"], (double)active_status["bytes_per_second"]);
	}
}

void HTTPFileDownloader::_emit_completion_signals() {
	Vector<DownloadItem> completed_items;
	Result result = RESULT_SUCCESS;

	{
		MutexLock lock(state_mutex);
		if (completion_emitted) {
			return;
		}
		completion_emitted = true;
		completed_items = items;
		result = batch_result;
	}

	for (const DownloadItem &item : completed_items) {
		if (item.status == DOWNLOAD_STATUS_COMPLETED || item.status == DOWNLOAD_STATUS_FAILED || item.status == DOWNLOAD_STATUS_CANCELED) {
			emit_signal(SNAME("download_completed"), item.id, item.url, item.path, item.result, item.response_code);
		}
	}
	emit_signal(SNAME("batch_completed"), result);
}

void HTTPFileDownloader::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_INTERNAL_PROCESS: {
			if (running.is_set()) {
				_emit_progress_signal();
			}

			if (thread_done.is_set()) {
				if (coordinator_thread.is_started()) {
					coordinator_thread.wait_to_finish();
				}
				thread_done.clear();
				last_progress_signal_usec = 0;
				_emit_progress_signal();
				running.clear();
				finished.set();
				set_process_internal(false);
				_emit_completion_signals();
			}
		} break;

		case NOTIFICATION_EXIT_TREE: {
			if (running.is_set()) {
				cancel_downloads();
			}
		} break;
	}
}

Error HTTPFileDownloader::download_file(const String &p_url, const String &p_path, const Vector<String> &p_custom_headers) {
	ERR_FAIL_COND_V(p_url.is_empty(), ERR_INVALID_PARAMETER);
	ERR_FAIL_COND_V(p_path.is_empty(), ERR_INVALID_PARAMETER);

	DownloadItem item;
	item.id = 0;
	item.url = p_url;
	item.path = p_path;
	item.headers = p_custom_headers;

	Error err = _validate_headers(item.headers);
	if (err != OK) {
		return err;
	}

	Vector<DownloadItem> new_items;
	new_items.push_back(item);
	return _start_downloads(new_items);
}

Error HTTPFileDownloader::download_files(const TypedArray<Dictionary> &p_downloads, const Vector<String> &p_shared_headers) {
	ERR_FAIL_COND_V(p_downloads.size() == 0, ERR_INVALID_PARAMETER);

	Error err = _validate_headers(p_shared_headers);
	if (err != OK) {
		return err;
	}

	Vector<DownloadItem> new_items;
	for (int i = 0; i < p_downloads.size(); i++) {
		Dictionary download = p_downloads[i];
		ERR_FAIL_COND_V_MSG(!download.has("url"), ERR_INVALID_PARAMETER, "Each download dictionary must contain a 'url' key.");
		ERR_FAIL_COND_V_MSG(!download.has("path"), ERR_INVALID_PARAMETER, "Each download dictionary must contain a 'path' key.");

		DownloadItem item;
		item.id = i;
		item.url = String(download["url"]);
		item.path = String(download["path"]);
		item.headers = p_shared_headers;
		ERR_FAIL_COND_V(item.url.is_empty(), ERR_INVALID_PARAMETER);
		ERR_FAIL_COND_V(item.path.is_empty(), ERR_INVALID_PARAMETER);

		if (download.has("headers")) {
			err = _append_variant_headers(download["headers"], &item.headers);
			if (err != OK) {
				return err;
			}
		}

		err = _validate_headers(item.headers);
		if (err != OK) {
			return err;
		}

		new_items.push_back(item);
	}

	return _start_downloads(new_items);
}

void HTTPFileDownloader::cancel_downloads() {
	if (!running.is_set()) {
		return;
	}

	cancel_requested.set();

	if (coordinator_thread.is_started()) {
		coordinator_thread.wait_to_finish();
	}

	thread_done.clear();
	running.clear();
	finished.set();
	set_process_internal(false);
	_emit_completion_signals();
}

bool HTTPFileDownloader::is_downloading() const {
	return running.is_set();
}

bool HTTPFileDownloader::is_finished() const {
	return finished.is_set();
}

double HTTPFileDownloader::get_progress() const {
	const uint64_t now = OS::get_singleton()->get_ticks_usec();
	MutexLock lock(state_mutex);
	return _get_progress_unlocked(now);
}

int64_t HTTPFileDownloader::get_downloaded_bytes() const {
	MutexLock lock(state_mutex);
	return _get_downloaded_bytes_unlocked();
}

int64_t HTTPFileDownloader::get_total_bytes() const {
	MutexLock lock(state_mutex);
	return _get_total_bytes_unlocked();
}

double HTTPFileDownloader::get_elapsed_time() const {
	const uint64_t now = OS::get_singleton()->get_ticks_usec();
	MutexLock lock(state_mutex);
	return _get_elapsed_time_unlocked(now);
}

double HTTPFileDownloader::get_estimated_remaining_time() const {
	const uint64_t now = OS::get_singleton()->get_ticks_usec();
	MutexLock lock(state_mutex);
	return _get_estimated_remaining_time_unlocked(now);
}

double HTTPFileDownloader::get_bytes_per_second() const {
	const uint64_t now = OS::get_singleton()->get_ticks_usec();
	MutexLock lock(state_mutex);
	return _get_bytes_per_second_unlocked(now);
}

int HTTPFileDownloader::get_download_count() const {
	MutexLock lock(state_mutex);
	return items.size();
}

int HTTPFileDownloader::get_active_download_index() const {
	MutexLock lock(state_mutex);
	return active_download_index;
}

Dictionary HTTPFileDownloader::get_download_status(int p_index) const {
	const uint64_t now = OS::get_singleton()->get_ticks_usec();
	MutexLock lock(state_mutex);
	ERR_FAIL_INDEX_V(p_index, items.size(), Dictionary());
	return _get_item_status_unlocked(items[p_index], now);
}

TypedArray<Dictionary> HTTPFileDownloader::get_downloads_status() const {
	const uint64_t now = OS::get_singleton()->get_ticks_usec();
	TypedArray<Dictionary> output;

	MutexLock lock(state_mutex);
	for (const DownloadItem &item : items) {
		output.push_back(_get_item_status_unlocked(item, now));
	}

	return output;
}

void HTTPFileDownloader::set_thread_count(int p_thread_count) {
	ERR_FAIL_COND(running.is_set());
	ERR_FAIL_COND(p_thread_count < 0);
	thread_count = MIN(p_thread_count, MAX_THREAD_LIMIT);
}

int HTTPFileDownloader::get_thread_count() const {
	return thread_count;
}

void HTTPFileDownloader::set_max_thread_count(int p_max_thread_count) {
	ERR_FAIL_COND(running.is_set());
	ERR_FAIL_COND(p_max_thread_count < 1);
	max_thread_count = CLAMP(p_max_thread_count, 1, MAX_THREAD_LIMIT);
}

int HTTPFileDownloader::get_max_thread_count() const {
	return max_thread_count;
}

void HTTPFileDownloader::set_min_parallel_size(int64_t p_bytes) {
	ERR_FAIL_COND(running.is_set());
	ERR_FAIL_COND(p_bytes < 0);
	min_parallel_size = p_bytes;
}

int64_t HTTPFileDownloader::get_min_parallel_size() const {
	return min_parallel_size;
}

void HTTPFileDownloader::set_download_chunk_size(int p_bytes) {
	ERR_FAIL_COND(running.is_set());
	ERR_FAIL_COND(p_bytes < 256 || p_bytes > (1 << 24));
	download_chunk_size = p_bytes;
}

int HTTPFileDownloader::get_download_chunk_size() const {
	return download_chunk_size;
}

void HTTPFileDownloader::set_max_redirects(int p_max_redirects) {
	ERR_FAIL_COND(running.is_set());
	ERR_FAIL_COND(p_max_redirects < 0);
	max_redirects = p_max_redirects;
}

int HTTPFileDownloader::get_max_redirects() const {
	return max_redirects;
}

void HTTPFileDownloader::set_progress_update_interval(double p_interval) {
	ERR_FAIL_COND(p_interval < 0.0);
	progress_update_interval = p_interval;
}

double HTTPFileDownloader::get_progress_update_interval() const {
	return progress_update_interval;
}

void HTTPFileDownloader::set_keep_partial_files(bool p_keep) {
	ERR_FAIL_COND(running.is_set());
	keep_partial_files = p_keep;
}

bool HTTPFileDownloader::is_keep_partial_files_enabled() const {
	return keep_partial_files;
}

void HTTPFileDownloader::set_stop_on_error(bool p_stop) {
	ERR_FAIL_COND(running.is_set());
	stop_on_error = p_stop;
}

bool HTTPFileDownloader::is_stop_on_error_enabled() const {
	return stop_on_error;
}

void HTTPFileDownloader::set_http_proxy(const String &p_host, int p_port) {
	ERR_FAIL_COND(running.is_set());
	http_proxy_host = p_host;
	http_proxy_port = p_port;
}

void HTTPFileDownloader::set_https_proxy(const String &p_host, int p_port) {
	ERR_FAIL_COND(running.is_set());
	https_proxy_host = p_host;
	https_proxy_port = p_port;
}

void HTTPFileDownloader::set_tls_options(const Ref<TLSOptions> &p_options) {
	ERR_FAIL_COND(running.is_set());
	ERR_FAIL_COND(p_options.is_null() || p_options->is_server());
	tls_options = p_options;
}

void HTTPFileDownloader::_bind_methods() {
	ClassDB::bind_method(D_METHOD("download_file", "url", "path", "custom_headers"), &HTTPFileDownloader::download_file, DEFVAL(PackedStringArray()));
	ClassDB::bind_method(D_METHOD("download_files", "downloads", "shared_headers"), &HTTPFileDownloader::download_files, DEFVAL(PackedStringArray()));
	ClassDB::bind_method(D_METHOD("cancel_downloads"), &HTTPFileDownloader::cancel_downloads);

	ClassDB::bind_method(D_METHOD("is_downloading"), &HTTPFileDownloader::is_downloading);
	ClassDB::bind_method(D_METHOD("is_finished"), &HTTPFileDownloader::is_finished);
	ClassDB::bind_method(D_METHOD("get_progress"), &HTTPFileDownloader::get_progress);
	ClassDB::bind_method(D_METHOD("get_downloaded_bytes"), &HTTPFileDownloader::get_downloaded_bytes);
	ClassDB::bind_method(D_METHOD("get_total_bytes"), &HTTPFileDownloader::get_total_bytes);
	ClassDB::bind_method(D_METHOD("get_elapsed_time"), &HTTPFileDownloader::get_elapsed_time);
	ClassDB::bind_method(D_METHOD("get_estimated_remaining_time"), &HTTPFileDownloader::get_estimated_remaining_time);
	ClassDB::bind_method(D_METHOD("get_bytes_per_second"), &HTTPFileDownloader::get_bytes_per_second);
	ClassDB::bind_method(D_METHOD("get_download_count"), &HTTPFileDownloader::get_download_count);
	ClassDB::bind_method(D_METHOD("get_active_download_index"), &HTTPFileDownloader::get_active_download_index);
	ClassDB::bind_method(D_METHOD("get_download_status", "index"), &HTTPFileDownloader::get_download_status);
	ClassDB::bind_method(D_METHOD("get_downloads_status"), &HTTPFileDownloader::get_downloads_status);

	ClassDB::bind_method(D_METHOD("set_thread_count", "thread_count"), &HTTPFileDownloader::set_thread_count);
	ClassDB::bind_method(D_METHOD("get_thread_count"), &HTTPFileDownloader::get_thread_count);
	ClassDB::bind_method(D_METHOD("set_max_thread_count", "max_thread_count"), &HTTPFileDownloader::set_max_thread_count);
	ClassDB::bind_method(D_METHOD("get_max_thread_count"), &HTTPFileDownloader::get_max_thread_count);
	ClassDB::bind_method(D_METHOD("set_min_parallel_size", "bytes"), &HTTPFileDownloader::set_min_parallel_size);
	ClassDB::bind_method(D_METHOD("get_min_parallel_size"), &HTTPFileDownloader::get_min_parallel_size);
	ClassDB::bind_method(D_METHOD("set_download_chunk_size", "bytes"), &HTTPFileDownloader::set_download_chunk_size);
	ClassDB::bind_method(D_METHOD("get_download_chunk_size"), &HTTPFileDownloader::get_download_chunk_size);
	ClassDB::bind_method(D_METHOD("set_max_redirects", "max_redirects"), &HTTPFileDownloader::set_max_redirects);
	ClassDB::bind_method(D_METHOD("get_max_redirects"), &HTTPFileDownloader::get_max_redirects);
	ClassDB::bind_method(D_METHOD("set_progress_update_interval", "interval"), &HTTPFileDownloader::set_progress_update_interval);
	ClassDB::bind_method(D_METHOD("get_progress_update_interval"), &HTTPFileDownloader::get_progress_update_interval);
	ClassDB::bind_method(D_METHOD("set_keep_partial_files", "keep"), &HTTPFileDownloader::set_keep_partial_files);
	ClassDB::bind_method(D_METHOD("is_keep_partial_files_enabled"), &HTTPFileDownloader::is_keep_partial_files_enabled);
	ClassDB::bind_method(D_METHOD("set_stop_on_error", "stop"), &HTTPFileDownloader::set_stop_on_error);
	ClassDB::bind_method(D_METHOD("is_stop_on_error_enabled"), &HTTPFileDownloader::is_stop_on_error_enabled);

	ClassDB::bind_method(D_METHOD("set_http_proxy", "host", "port"), &HTTPFileDownloader::set_http_proxy);
	ClassDB::bind_method(D_METHOD("set_https_proxy", "host", "port"), &HTTPFileDownloader::set_https_proxy);
	ClassDB::bind_method(D_METHOD("set_tls_options", "client_options"), &HTTPFileDownloader::set_tls_options);

	ADD_PROPERTY(PropertyInfo(Variant::INT, "thread_count", PROPERTY_HINT_RANGE, "0,64,1"), "set_thread_count", "get_thread_count");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "max_thread_count", PROPERTY_HINT_RANGE, "1,64,1"), "set_max_thread_count", "get_max_thread_count");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "min_parallel_size", PROPERTY_HINT_RANGE, "0,1073741824,1,or_greater,suffix:B"), "set_min_parallel_size", "get_min_parallel_size");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "download_chunk_size", PROPERTY_HINT_RANGE, "256,16777216,1,suffix:B"), "set_download_chunk_size", "get_download_chunk_size");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "max_redirects", PROPERTY_HINT_RANGE, "0,64"), "set_max_redirects", "get_max_redirects");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "progress_update_interval", PROPERTY_HINT_RANGE, "0,10,0.01,or_greater,suffix:s"), "set_progress_update_interval", "get_progress_update_interval");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "keep_partial_files"), "set_keep_partial_files", "is_keep_partial_files_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "stop_on_error"), "set_stop_on_error", "is_stop_on_error_enabled");

	ADD_SIGNAL(MethodInfo("download_progressed", PropertyInfo(Variant::INT, "id"), PropertyInfo(Variant::STRING, "url"), PropertyInfo(Variant::INT, "downloaded_bytes"), PropertyInfo(Variant::INT, "total_bytes"), PropertyInfo(Variant::FLOAT, "progress"), PropertyInfo(Variant::FLOAT, "elapsed_time"), PropertyInfo(Variant::FLOAT, "estimated_remaining_time"), PropertyInfo(Variant::FLOAT, "bytes_per_second")));
	ADD_SIGNAL(MethodInfo("batch_progressed", PropertyInfo(Variant::INT, "downloaded_bytes"), PropertyInfo(Variant::INT, "total_bytes"), PropertyInfo(Variant::FLOAT, "progress"), PropertyInfo(Variant::FLOAT, "elapsed_time"), PropertyInfo(Variant::FLOAT, "estimated_remaining_time"), PropertyInfo(Variant::FLOAT, "bytes_per_second")));
	ADD_SIGNAL(MethodInfo("download_completed", PropertyInfo(Variant::INT, "id"), PropertyInfo(Variant::STRING, "url"), PropertyInfo(Variant::STRING, "path"), PropertyInfo(Variant::INT, "result"), PropertyInfo(Variant::INT, "response_code")));
	ADD_SIGNAL(MethodInfo("batch_completed", PropertyInfo(Variant::INT, "result")));

	BIND_ENUM_CONSTANT(RESULT_SUCCESS);
	BIND_ENUM_CONSTANT(RESULT_CANT_CONNECT);
	BIND_ENUM_CONSTANT(RESULT_CANT_RESOLVE);
	BIND_ENUM_CONSTANT(RESULT_CONNECTION_ERROR);
	BIND_ENUM_CONSTANT(RESULT_TLS_HANDSHAKE_ERROR);
	BIND_ENUM_CONSTANT(RESULT_NO_RESPONSE);
	BIND_ENUM_CONSTANT(RESULT_HTTP_ERROR);
	BIND_ENUM_CONSTANT(RESULT_DOWNLOAD_FILE_CANT_OPEN);
	BIND_ENUM_CONSTANT(RESULT_DOWNLOAD_FILE_WRITE_ERROR);
	BIND_ENUM_CONSTANT(RESULT_REDIRECT_LIMIT_REACHED);
	BIND_ENUM_CONSTANT(RESULT_CANCELED);
	BIND_ENUM_CONSTANT(RESULT_INVALID_URL);
	BIND_ENUM_CONSTANT(RESULT_RANGE_NOT_SUPPORTED);
	BIND_ENUM_CONSTANT(RESULT_UNAVAILABLE);

	get_gdtype_static_mutable().bind_integer_constant(StringName("DownloadStatus"), "STATUS_PENDING", DOWNLOAD_STATUS_PENDING);
	get_gdtype_static_mutable().bind_integer_constant(StringName("DownloadStatus"), "STATUS_PREPARING", DOWNLOAD_STATUS_PREPARING);
	get_gdtype_static_mutable().bind_integer_constant(StringName("DownloadStatus"), "STATUS_DOWNLOADING", DOWNLOAD_STATUS_DOWNLOADING);
	get_gdtype_static_mutable().bind_integer_constant(StringName("DownloadStatus"), "STATUS_COMPLETED", DOWNLOAD_STATUS_COMPLETED);
	get_gdtype_static_mutable().bind_integer_constant(StringName("DownloadStatus"), "STATUS_FAILED", DOWNLOAD_STATUS_FAILED);
	get_gdtype_static_mutable().bind_integer_constant(StringName("DownloadStatus"), "STATUS_CANCELED", DOWNLOAD_STATUS_CANCELED);
}

HTTPFileDownloader::HTTPFileDownloader() {
	tls_options = TLSOptions::client();
}

HTTPFileDownloader::~HTTPFileDownloader() {
	if (running.is_set()) {
		cancel_requested.set();
		if (coordinator_thread.is_started()) {
			coordinator_thread.wait_to_finish();
		}
		running.clear();
		finished.set();
	}
}
