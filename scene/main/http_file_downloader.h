/**************************************************************************/
/*  http_file_downloader.h                                                */
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

#include "core/io/http_client.h"
#include "core/os/mutex.h"
#include "core/os/thread.h"
#include "core/templates/safe_refcount.h"
#include "core/variant/dictionary.h"
#include "core/variant/typed_array.h"
#include "core/variant/variant.h"
#include "scene/main/node.h"

class FileAccess;

class HTTPFileDownloader : public Node {
	GDCLASS(HTTPFileDownloader, Node);

public:
	enum Result {
		RESULT_SUCCESS,
		RESULT_CANT_CONNECT,
		RESULT_CANT_RESOLVE,
		RESULT_CONNECTION_ERROR,
		RESULT_TLS_HANDSHAKE_ERROR,
		RESULT_NO_RESPONSE,
		RESULT_HTTP_ERROR,
		RESULT_DOWNLOAD_FILE_CANT_OPEN,
		RESULT_DOWNLOAD_FILE_WRITE_ERROR,
		RESULT_REDIRECT_LIMIT_REACHED,
		RESULT_CANCELED,
		RESULT_INVALID_URL,
		RESULT_RANGE_NOT_SUPPORTED,
		RESULT_UNAVAILABLE,
	};

	enum DownloadStatus {
		STATUS_PENDING,
		STATUS_PREPARING,
		STATUS_DOWNLOADING,
		STATUS_COMPLETED,
		STATUS_FAILED,
		STATUS_CANCELED,
	};

private:
	static constexpr int MAX_THREAD_LIMIT = 64;
	static constexpr int DEFAULT_MAX_THREAD_COUNT = 8;
	static constexpr int64_t DEFAULT_MIN_PARALLEL_SIZE = 8 * 1024 * 1024;
	static constexpr int DEFAULT_DOWNLOAD_CHUNK_SIZE = 256 * 1024;

	struct ParsedURL {
		String source_url;
		String host;
		String request;
		int port = 80;
		bool use_tls = false;
	};

	struct ResponseInfo {
		int response_code = 0;
		Vector<String> headers;
		int64_t total_bytes = -1;
		bool range_supported = false;
		String final_url;
	};

	struct DownloadItem {
		int id = -1;
		String url;
		String path;
		Vector<String> headers;
		DownloadStatus status = STATUS_PENDING;
		Result result = RESULT_SUCCESS;
		int response_code = 0;
		Vector<String> response_headers;
		int64_t downloaded_bytes = 0;
		int64_t total_bytes = -1;
		int assigned_thread_count = 0;
		uint64_t started_usec = 0;
		uint64_t finished_usec = 0;
	};

	struct SegmentDownload {
		HTTPFileDownloader *downloader = nullptr;
		int item_index = -1;
		String url;
		Vector<String> headers;
		Ref<FileAccess> file;
		int64_t range_start = 0;
		int64_t range_end = 0;
		int64_t write_offset = 0;
		Result result = RESULT_SUCCESS;
		int response_code = 0;
	};

	mutable Mutex state_mutex;
	Mutex file_mutex;
	Vector<DownloadItem> items;

	Thread coordinator_thread;
	SafeFlag running;
	SafeFlag finished;
	SafeFlag cancel_requested;
	SafeFlag thread_done;

	Ref<TLSOptions> tls_options;
	String http_proxy_host;
	int http_proxy_port = -1;
	String https_proxy_host;
	int https_proxy_port = -1;

	int thread_count = 0;
	int max_thread_count = DEFAULT_MAX_THREAD_COUNT;
	int64_t min_parallel_size = DEFAULT_MIN_PARALLEL_SIZE;
	int download_chunk_size = DEFAULT_DOWNLOAD_CHUNK_SIZE;
	int max_redirects = 8;
	double progress_update_interval = 0.1;
	bool keep_partial_files = false;
	bool stop_on_error = false;

	int active_download_index = -1;
	Result batch_result = RESULT_SUCCESS;
	uint64_t batch_started_usec = 0;
	uint64_t batch_finished_usec = 0;
	uint64_t last_progress_signal_usec = 0;
	bool completion_emitted = true;

	static void _coordinator_thread_func(void *p_userdata);
	static void _segment_thread_func(void *p_userdata);

	Error _start_downloads(const Vector<DownloadItem> &p_items);
	void _coordinator_loop();
	Result _download_item(int p_index);
	Result _download_item_single(int p_index, const String &p_url);
	Result _download_item_parallel(int p_index, const String &p_url, int p_thread_count);
	Result _probe_item(int p_index, ResponseInfo *r_response);
	Result _perform_request(const String &p_url, HTTPClient::Method p_method, const Vector<String> &p_headers, int64_t p_range_start, int64_t p_range_end, bool p_headers_only, const Ref<FileAccess> &p_output_file, int64_t p_write_offset, int p_item_index, bool p_require_partial_response, ResponseInfo *r_response, int64_t *r_written);

	Error _parse_url(const String &p_url, ParsedURL *r_url) const;
	Vector<String> _make_request_headers(const Vector<String> &p_headers, int64_t p_range_start, int64_t p_range_end) const;
	Error _validate_headers(const Vector<String> &p_headers) const;
	Error _append_variant_headers(const Variant &p_headers, Vector<String> *r_headers) const;
	String _get_header_value(const Vector<String> &p_headers, const String &p_header_name) const;
	bool _is_redirect_response(int p_response_code) const;
	bool _is_success_response(int p_response_code) const;
	String _resolve_redirect_url(const ParsedURL &p_base_url, const String &p_location) const;
	int64_t _get_total_bytes_from_headers(int p_response_code, const Vector<String> &p_headers) const;
	Ref<FileAccess> _open_output_file(const String &p_path, bool p_read_write, int64_t p_size, Result *r_result) const;
	int _get_effective_thread_count(int64_t p_total_bytes) const;

	void _set_active_download(int p_index);
	void _set_item_status(int p_index, DownloadStatus p_status, Result p_result = RESULT_SUCCESS, int p_response_code = 0);
	void _set_item_response(int p_index, const ResponseInfo &p_response);
	void _set_item_total_bytes(int p_index, int64_t p_total_bytes);
	void _set_item_thread_count(int p_index, int p_thread_count);
	void _reset_item_downloaded_bytes(int p_index);
	void _add_downloaded_bytes(int p_index, int64_t p_bytes);
	void _set_batch_finished(Result p_result);

	Dictionary _get_item_status_unlocked(const DownloadItem &p_item, uint64_t p_now_usec) const;
	int64_t _get_downloaded_bytes_unlocked() const;
	int64_t _get_total_bytes_unlocked(bool *r_has_unknown = nullptr) const;
	double _get_progress_unlocked(uint64_t p_now_usec) const;
	double _get_elapsed_time_unlocked(uint64_t p_now_usec) const;
	double _get_bytes_per_second_unlocked(uint64_t p_now_usec) const;
	double _get_estimated_remaining_time_unlocked(uint64_t p_now_usec) const;

	void _emit_progress_signal();
	void _emit_completion_signals();

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	Error download_file(const String &p_url, const String &p_path, const Vector<String> &p_custom_headers = Vector<String>());
	Error download_files(const TypedArray<Dictionary> &p_downloads, const Vector<String> &p_shared_headers = Vector<String>());
	void cancel_downloads();

	bool is_downloading() const;
	bool is_finished() const;

	double get_progress() const;
	int64_t get_downloaded_bytes() const;
	int64_t get_total_bytes() const;
	double get_elapsed_time() const;
	double get_estimated_remaining_time() const;
	double get_bytes_per_second() const;

	int get_download_count() const;
	int get_active_download_index() const;
	Dictionary get_download_status(int p_index) const;
	TypedArray<Dictionary> get_downloads_status() const;

	void set_thread_count(int p_thread_count);
	int get_thread_count() const;
	void set_max_thread_count(int p_max_thread_count);
	int get_max_thread_count() const;
	void set_min_parallel_size(int64_t p_bytes);
	int64_t get_min_parallel_size() const;
	void set_download_chunk_size(int p_bytes);
	int get_download_chunk_size() const;
	void set_max_redirects(int p_max_redirects);
	int get_max_redirects() const;
	void set_progress_update_interval(double p_interval);
	double get_progress_update_interval() const;
	void set_keep_partial_files(bool p_keep);
	bool is_keep_partial_files_enabled() const;
	void set_stop_on_error(bool p_stop);
	bool is_stop_on_error_enabled() const;

	void set_http_proxy(const String &p_host, int p_port);
	void set_https_proxy(const String &p_host, int p_port);
	void set_tls_options(const Ref<TLSOptions> &p_options);

	HTTPFileDownloader();
	~HTTPFileDownloader();
};

VARIANT_ENUM_CAST(HTTPFileDownloader::Result);
VARIANT_ENUM_CAST(HTTPFileDownloader::DownloadStatus);
