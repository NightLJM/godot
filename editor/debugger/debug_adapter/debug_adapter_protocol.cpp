/*************************************************************************/
/*  debug_adapter_protocol.cpp                                           */
/*************************************************************************/
/*                       This file is part of:                           */
/*                           GODOT ENGINE                                */
/*                      https://godotengine.org                          */
/*************************************************************************/
/* Copyright (c) 2007-2021 Juan Linietsky, Ariel Manzur.                 */
/* Copyright (c) 2014-2021 Godot Engine contributors (cf. AUTHORS.md).   */
/*                                                                       */
/* Permission is hereby granted, free of charge, to any person obtaining */
/* a copy of this software and associated documentation files (the       */
/* "Software"), to deal in the Software without restriction, including   */
/* without limitation the rights to use, copy, modify, merge, publish,   */
/* distribute, sublicense, and/or sell copies of the Software, and to    */
/* permit persons to whom the Software is furnished to do so, subject to */
/* the following conditions:                                             */
/*                                                                       */
/* The above copyright notice and this permission notice shall be        */
/* included in all copies or substantial portions of the Software.       */
/*                                                                       */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,       */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF    */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.*/
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY  */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,  */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE     */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                */
/*************************************************************************/

#include "debug_adapter_protocol.h"

#include "core/config/project_settings.h"
#include "core/debugger/debugger_marshalls.h"
#include "core/io/json.h"
#include "editor/debugger/script_editor_debugger.h"
#include "editor/doc_tools.h"
#include "editor/editor_log.h"
#include "editor/editor_node.h"

DebugAdapterProtocol *DebugAdapterProtocol::singleton = nullptr;

Error DAPeer::handle_data() {
	int read = 0;
	// Read headers
	if (!has_header) {
		if (!connection->get_available_bytes()) {
			return OK;
		}
		while (true) {
			if (req_pos >= DAP_MAX_BUFFER_SIZE) {
				req_pos = 0;
				ERR_FAIL_COND_V_MSG(true, ERR_OUT_OF_MEMORY, "Response header too big");
			}
			Error err = connection->get_partial_data(&req_buf[req_pos], 1, read);
			if (err != OK) {
				return FAILED;
			} else if (read != 1) { // Busy, wait until next poll
				return ERR_BUSY;
			}
			char *r = (char *)req_buf;
			int l = req_pos;

			// End of headers
			if (l > 3 && r[l] == '\n' && r[l - 1] == '\r' && r[l - 2] == '\n' && r[l - 3] == '\r') {
				r[l - 3] = '\0'; // Null terminate to read string
				String header;
				header.parse_utf8(r);
				content_length = header.substr(16).to_int();
				has_header = true;
				req_pos = 0;
				break;
			}
			req_pos++;
		}
	}
	if (has_header) {
		while (req_pos < content_length) {
			if (content_length >= DAP_MAX_BUFFER_SIZE) {
				req_pos = 0;
				has_header = false;
				ERR_FAIL_COND_V_MSG(req_pos >= DAP_MAX_BUFFER_SIZE, ERR_OUT_OF_MEMORY, "Response content too big");
			}
			Error err = connection->get_partial_data(&req_buf[req_pos], content_length - req_pos, read);
			if (err != OK) {
				return FAILED;
			} else if (read < content_length - req_pos) {
				return ERR_BUSY;
			}
			req_pos += read;
		}

		// Parse data
		String msg;
		msg.parse_utf8((const char *)req_buf, req_pos);

		// Response
		if (DebugAdapterProtocol::get_singleton()->process_message(msg)) {
			// Reset to read again
			req_pos = 0;
			has_header = false;
		}
	}
	return OK;
}

Error DAPeer::send_data() {
	while (res_queue.size()) {
		Dictionary data = res_queue.front()->get();
		if (!data.has("seq")) {
			data["seq"] = ++seq;
		}
		String formatted_data = format_output(data);

		int data_sent = 0;
		while (data_sent < formatted_data.length()) {
			int curr_sent = 0;
			Error err = connection->put_partial_data((const uint8_t *)formatted_data.utf8().get_data(), formatted_data.size() - data_sent - 1, curr_sent);
			if (err != OK) {
				return err;
			}
			data_sent += curr_sent;
		}
		res_queue.pop_front();
	}
	return OK;
}

String DAPeer::format_output(const Dictionary &p_params) const {
	String response = Variant(p_params).to_json_string();
	String header = "Content-Length: ";
	CharString charstr = response.utf8();
	size_t len = charstr.length();
	header += itos(len);
	header += "\r\n\r\n";

	return header + response;
}

Error DebugAdapterProtocol::on_client_connected() {
	ERR_FAIL_COND_V_MSG(clients.size() >= DAP_MAX_CLIENTS, FAILED, "Max client limits reached");

	Ref<StreamPeerTCP> tcp_peer = server->take_connection();
	tcp_peer->set_no_delay(true);
	Ref<DAPeer> peer = memnew(DAPeer);
	peer->connection = tcp_peer;
	clients.push_back(peer);

	EditorDebuggerNode::get_singleton()->get_default_debugger()->set_move_to_foreground(false);
	EditorNode::get_log()->add_message("[DAP] Connection Taken", EditorLog::MSG_TYPE_EDITOR);
	return OK;
}

void DebugAdapterProtocol::on_client_disconnected(const Ref<DAPeer> &p_peer) {
	clients.erase(p_peer);
	if (!clients.size()) {
		reset_ids();
		EditorDebuggerNode::get_singleton()->get_default_debugger()->set_move_to_foreground(true);
	}
	EditorNode::get_log()->add_message("[DAP] Disconnected", EditorLog::MSG_TYPE_EDITOR);
}

void DebugAdapterProtocol::reset_current_info() {
	_current_request = "";
	_current_peer.unref();
}

void DebugAdapterProtocol::reset_ids() {
	breakpoint_id = 0;
	breakpoint_list.clear();

	reset_stack_info();
}

void DebugAdapterProtocol::reset_stack_info() {
	stackframe_id = 0;
	variable_id = 1;

	stackframe_list.clear();
	variable_list.clear();
}

bool DebugAdapterProtocol::process_message(const String &p_text) {
	JSON json;
	ERR_FAIL_COND_V_MSG(json.parse(p_text) != OK, true, "Mal-formed message!");
	Dictionary params = json.get_data();
	bool completed = true;

	// Append "req_" to any command received; prevents name clash with existing functions, and possibly exploiting
	String command = "req_" + (String)params["command"];
	if (parser->has_method(command)) {
		_current_request = params["command"];

		Array args;
		args.push_back(params);
		Dictionary response = parser->callv(command, args);
		if (!response.is_empty()) {
			_current_peer->res_queue.push_front(response);
		} else {
			completed = false;
		}
	}

	reset_current_info();
	return completed;
}

void DebugAdapterProtocol::notify_initialized() {
	Dictionary event = parser->ev_initialized();
	_current_peer->res_queue.push_back(event);
}

void DebugAdapterProtocol::notify_process() {
	String launch_mode = _current_request.is_empty() ? "launch" : _current_request;

	Dictionary event = parser->ev_process(launch_mode);
	for (List<Ref<DAPeer>>::Element *E = clients.front(); E; E = E->next()) {
		E->get()->res_queue.push_back(event);
	}
}

void DebugAdapterProtocol::notify_terminated() {
	Dictionary event = parser->ev_terminated();
	for (List<Ref<DAPeer>>::Element *E = clients.front(); E; E = E->next()) {
		if (_current_request == "launch" && _current_peer == E->get()) {
			continue;
		}
		E->get()->res_queue.push_back(event);
	}
}

void DebugAdapterProtocol::notify_exited(const int &p_exitcode) {
	Dictionary event = parser->ev_exited(p_exitcode);
	for (List<Ref<DAPeer>>::Element *E = clients.front(); E; E = E->next()) {
		if (_current_request == "launch" && _current_peer == E->get()) {
			continue;
		}
		E->get()->res_queue.push_back(event);
	}
}

void DebugAdapterProtocol::notify_stopped_paused() {
	Dictionary event = parser->ev_stopped_paused();
	for (List<Ref<DAPeer>>::Element *E = clients.front(); E; E = E->next()) {
		E->get()->res_queue.push_back(event);
	}
}

void DebugAdapterProtocol::notify_stopped_exception(const String &p_error) {
	Dictionary event = parser->ev_stopped_exception(p_error);
	for (List<Ref<DAPeer>>::Element *E = clients.front(); E; E = E->next()) {
		E->get()->res_queue.push_back(event);
	}
}

void DebugAdapterProtocol::notify_stopped_breakpoint(const int &p_id) {
	Dictionary event = parser->ev_stopped_breakpoint(p_id);
	for (List<Ref<DAPeer>>::Element *E = clients.front(); E; E = E->next()) {
		E->get()->res_queue.push_back(event);
	}
}

void DebugAdapterProtocol::notify_stopped_step() {
	Dictionary event = parser->ev_stopped_step();
	for (List<Ref<DAPeer>>::Element *E = clients.front(); E; E = E->next()) {
		E->get()->res_queue.push_back(event);
	}
}

void DebugAdapterProtocol::notify_continued() {
	Dictionary event = parser->ev_continued();
	for (List<Ref<DAPeer>>::Element *E = clients.front(); E; E = E->next()) {
		if (_current_request == "continue" && E->get() == _current_peer) {
			continue;
		}
		E->get()->res_queue.push_back(event);
	}

	reset_stack_info();
}

void DebugAdapterProtocol::notify_output(const String &p_message) {
	Dictionary event = parser->ev_output(p_message);
	for (List<Ref<DAPeer>>::Element *E = clients.front(); E; E = E->next()) {
		E->get()->res_queue.push_back(event);
	}
}

Array DebugAdapterProtocol::update_breakpoints(const String &p_path, const Array &p_lines) {
	Array updated_breakpoints;

	for (int i = 0; i < p_lines.size(); i++) {
		DAP::Breakpoint breakpoint;
		breakpoint.verified = true;
		breakpoint.source.path = p_path;
		breakpoint.source.compute_checksums();
		breakpoint.line = p_lines[i];

		List<DAP::Breakpoint>::Element *E = breakpoint_list.find(breakpoint);
		if (E) {
			breakpoint.id = E->get().id;
		} else {
			breakpoint.id = breakpoint_id++;
			breakpoint_list.push_back(breakpoint);
		}

		updated_breakpoints.push_back(breakpoint.to_json());
	}

	return updated_breakpoints;
}

void DebugAdapterProtocol::on_debug_paused() {
	if (EditorNode::get_singleton()->get_pause_button()->is_pressed()) {
		notify_stopped_paused();
	} else {
		notify_continued();
	}
}

void DebugAdapterProtocol::on_debug_stopped() {
	notify_exited();
	notify_terminated();
}

void DebugAdapterProtocol::on_debug_output(const String &p_message) {
	notify_output(p_message);
}

void DebugAdapterProtocol::on_debug_breaked(const bool &p_reallydid, const bool &p_can_debug, const String &p_reason, const bool &p_has_stackdump) {
	if (!p_reallydid) {
		notify_continued();
		return;
	}

	if (p_reason == "Breakpoint") {
		if (_stepping) {
			notify_stopped_step();
			_stepping = false;
		} else {
			_processing_breakpoint = true; // Wait for stack_dump to find where the breakpoint happened
		}
	} else {
		notify_stopped_exception(p_reason);
	}

	_processing_stackdump = p_has_stackdump;
}

void DebugAdapterProtocol::on_debug_stack_dump(const Array &p_stack_dump) {
	if (_processing_breakpoint && !p_stack_dump.is_empty()) {
		// Find existing breakpoint
		Dictionary d = p_stack_dump[0];
		DAP::Breakpoint breakpoint;
		breakpoint.source.path = ProjectSettings::get_singleton()->globalize_path(d["file"]);
		breakpoint.line = d["line"];

		List<DAP::Breakpoint>::Element *E = breakpoint_list.find(breakpoint);
		if (E) {
			notify_stopped_breakpoint(E->get().id);
		}

		_processing_breakpoint = false;
	}

	stackframe_id = 0;
	stackframe_list.clear();

	// Fill in stacktrace information
	for (int i = 0; i < p_stack_dump.size(); i++) {
		Dictionary stack_info = p_stack_dump[i];
		DAP::StackFrame stackframe;
		stackframe.id = stackframe_id++;
		stackframe.name = stack_info["function"];
		stackframe.line = stack_info["line"];
		stackframe.column = 0;
		stackframe.source.path = ProjectSettings::get_singleton()->globalize_path(stack_info["file"]);
		stackframe.source.compute_checksums();

		// Information for "Locals", "Members" and "Globals" variables respectively
		List<int> scope_ids;
		for (int j = 0; j < 3; j++) {
			scope_ids.push_back(variable_id++);
		}

		stackframe_list.insert(stackframe, scope_ids);
	}

	_current_frame = 0;
	_processing_stackdump = false;
}

void DebugAdapterProtocol::on_debug_stack_frame_vars(const int &p_size) {
	_remaining_vars = p_size;
	DAP::StackFrame frame;
	frame.id = _current_frame;
	ERR_FAIL_COND(!stackframe_list.has(frame));
	List<int> scope_ids = stackframe_list.find(frame)->value();
	for (List<int>::Element *E = scope_ids.front(); E; E = E->next()) {
		int variable_id = E->get();
		if (variable_list.has(variable_id)) {
			variable_list.find(variable_id)->value().clear();
		} else {
			variable_list.insert(variable_id, Array());
		}
	}
}

void DebugAdapterProtocol::on_debug_stack_frame_var(const Array &p_data) {
	DebuggerMarshalls::ScriptStackVariable stack_var;
	stack_var.deserialize(p_data);

	ERR_FAIL_COND(stackframe_list.is_empty());
	DAP::StackFrame frame;
	frame.id = _current_frame;

	List<int> scope_ids = stackframe_list.find(frame)->value();
	ERR_FAIL_COND(scope_ids.size() != 3);
	ERR_FAIL_INDEX(stack_var.type, 3);
	int variable_id = scope_ids[stack_var.type];

	DAP::Variable variable;

	variable.name = stack_var.name;
	variable.value = stack_var.value;
	variable.type = Variant::get_type_name(stack_var.value.get_type());

	variable_list.find(variable_id)->value().push_back(variable.to_json());
	_remaining_vars--;
}

void DebugAdapterProtocol::poll() {
	if (server->is_connection_available()) {
		on_client_connected();
	}
	List<Ref<DAPeer>> to_delete;
	for (List<Ref<DAPeer>>::Element *E = clients.front(); E; E = E->next()) {
		Ref<DAPeer> peer = E->get();
		StreamPeerTCP::Status status = peer->connection->get_status();
		if (status == StreamPeerTCP::STATUS_NONE || status == StreamPeerTCP::STATUS_ERROR) {
			to_delete.push_back(peer);
		} else {
			_current_peer = peer;
			Error err = peer->handle_data();
			if (err != OK && err != ERR_BUSY) {
				to_delete.push_back(peer);
			}
			err = peer->send_data();
			if (err != OK && err != ERR_BUSY) {
				to_delete.push_back(peer);
			}
		}
	}

	for (List<Ref<DAPeer>>::Element *E = to_delete.front(); E; E = E->next()) {
		on_client_disconnected(E->get());
	}
	to_delete.clear();
}

Error DebugAdapterProtocol::start(int p_port, const IPAddress &p_bind_ip) {
	_initialized = true;
	return server->listen(p_port, p_bind_ip);
}

void DebugAdapterProtocol::stop() {
	for (List<Ref<DAPeer>>::Element *E = clients.front(); E; E = E->next()) {
		E->get()->connection->disconnect_from_host();
	}

	clients.clear();
	server->stop();
	_initialized = false;
}

DebugAdapterProtocol::DebugAdapterProtocol() {
	server.instantiate();
	singleton = this;
	parser = memnew(DebugAdapterParser);

	reset_ids();

	EditorNode *node = EditorNode::get_singleton();
	node->get_pause_button()->connect("pressed", callable_mp(this, &DebugAdapterProtocol::on_debug_paused));

	EditorDebuggerNode *debugger_node = EditorDebuggerNode::get_singleton();
	debugger_node->get_default_debugger()->connect("stopped", callable_mp(this, &DebugAdapterProtocol::on_debug_stopped));
	debugger_node->get_default_debugger()->connect("output", callable_mp(this, &DebugAdapterProtocol::on_debug_output));
	debugger_node->get_default_debugger()->connect("breaked", callable_mp(this, &DebugAdapterProtocol::on_debug_breaked));
	debugger_node->get_default_debugger()->connect("stack_dump", callable_mp(this, &DebugAdapterProtocol::on_debug_stack_dump));
	debugger_node->get_default_debugger()->connect("stack_frame_vars", callable_mp(this, &DebugAdapterProtocol::on_debug_stack_frame_vars));
	debugger_node->get_default_debugger()->connect("stack_frame_var", callable_mp(this, &DebugAdapterProtocol::on_debug_stack_frame_var));
}

DebugAdapterProtocol::~DebugAdapterProtocol() {
	memdelete(parser);
}
