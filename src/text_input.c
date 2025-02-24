#define G_LOG_DOMAIN "phoc-text-input"

#include "config.h"

#include <assert.h>
#include <stdlib.h>
#include <wlr/util/log.h>
#include "seat.h"
#include "server.h"
#include "text_input.h"

static struct roots_text_input *relay_get_focusable_text_input(
		struct roots_input_method_relay *relay) {
	struct roots_text_input *text_input = NULL;
	wl_list_for_each(text_input, &relay->text_inputs, link) {
		if (text_input->pending_focused_surface) {
			return text_input;
		}
	}
	return NULL;
}

static struct roots_text_input *relay_get_focused_text_input(
		struct roots_input_method_relay *relay) {
	struct roots_text_input *text_input = NULL;
	wl_list_for_each(text_input, &relay->text_inputs, link) {
		if (text_input->input->focused_surface) {
			assert(text_input->pending_focused_surface == NULL);
			return text_input;
		}
	}
	return NULL;
}

static void handle_im_commit(struct wl_listener *listener, void *data) {
	struct roots_input_method_relay *relay = wl_container_of(listener, relay,
		input_method_commit);

	struct roots_text_input *text_input = relay_get_focused_text_input(relay);
	if (!text_input) {
		return;
	}
	struct wlr_input_method_v2 *context = data;
	assert(context == relay->input_method);
	if (context->current.preedit.text) {
		wlr_text_input_v3_send_preedit_string(text_input->input,
			context->current.preedit.text,
			context->current.preedit.cursor_begin,
			context->current.preedit.cursor_end);
	}
	if (context->current.commit_text) {
		wlr_text_input_v3_send_commit_string(text_input->input,
			context->current.commit_text);
	}
	if (context->current.delete.before_length
			|| context->current.delete.after_length) {
		wlr_text_input_v3_send_delete_surrounding_text(text_input->input,
			context->current.delete.before_length,
			context->current.delete.after_length);
	}
	wlr_text_input_v3_send_done(text_input->input);
}


static void text_input_clear_pending_focused_surface(
		struct roots_text_input *text_input) {
	wl_list_remove(&text_input->pending_focused_surface_destroy.link);
	wl_list_init(&text_input->pending_focused_surface_destroy.link);
	text_input->pending_focused_surface = NULL;
}

static void text_input_set_pending_focused_surface(
		struct roots_text_input *text_input, struct wlr_surface *surface) {
	text_input_clear_pending_focused_surface(text_input);
	g_assert(surface);
	text_input->pending_focused_surface = surface;
	wl_signal_add(&surface->events.destroy,
		&text_input->pending_focused_surface_destroy);
}

static void handle_im_destroy(struct wl_listener *listener, void *data) {
	struct roots_input_method_relay *relay = wl_container_of(listener, relay,
		input_method_destroy);
	struct wlr_input_method_v2 *context = data;
	assert(context == relay->input_method);
	relay->input_method = NULL;
	struct roots_text_input *text_input = relay_get_focused_text_input(relay);
	if (text_input) {
		// keyboard focus is still there, so keep the surface at hand in case
		// the input method returns
		assert(text_input->pending_focused_surface == NULL);
		text_input_set_pending_focused_surface(text_input,
			text_input->input->focused_surface);
		wlr_text_input_v3_send_leave(text_input->input);
	}
}

static bool text_input_is_focused(struct wlr_text_input_v3 *text_input) {
	// roots_input_method_relay_set_focus ensures
	// that focus sits on the single text input with focused_surface set.
	return text_input->focused_surface != NULL;
}

static void relay_send_im_done(struct roots_input_method_relay *relay,
		struct wlr_text_input_v3 *input) {
	struct wlr_input_method_v2 *input_method = relay->input_method;
	if (!input_method) {
		wlr_log(WLR_INFO, "Sending IM_DONE but im is gone");
		return;
	}
	if (!text_input_is_focused(input)) {
		// Don't let input method know about events from unfocused surfaces.
		return;
	}
	// TODO: only send each of those if they were modified
	if (input->active_features & WLR_TEXT_INPUT_V3_FEATURE_SURROUNDING_TEXT) {
		wlr_input_method_v2_send_surrounding_text(input_method,
			input->current.surrounding.text, input->current.surrounding.cursor,
			input->current.surrounding.anchor);
	}
	wlr_input_method_v2_send_text_change_cause(input_method,
		input->current.text_change_cause);
	if (input->active_features & WLR_TEXT_INPUT_v3_FEATURE_CONTENT_TYPE) {
		wlr_input_method_v2_send_content_type(input_method,
			input->current.content_type.hint, input->current.content_type.purpose);
	}
	wlr_input_method_v2_send_done(input_method);
	// TODO: pass intent, display popup size
}

static void handle_text_input_enable(struct wl_listener *listener, void *data) {
	struct roots_text_input *text_input = wl_container_of(listener, text_input,
		enable);
	struct roots_input_method_relay *relay = text_input->relay;
	if (relay->input_method == NULL) {
		wlr_log(WLR_INFO, "Enabling text input when input method is gone");
		return;
	}
	// relay_send_im_done protects from receiving unfocussed done,
	// but activate must be prevented too.
	// TODO: when enter happens?
	if (!text_input_is_focused(text_input->input)) {
		return;
	}
	wlr_input_method_v2_send_activate(relay->input_method);
	relay_send_im_done(relay, text_input->input);
}

static void handle_text_input_commit(struct wl_listener *listener,
		void *data) {
	struct roots_text_input *text_input = wl_container_of(listener, text_input,
		commit);
	struct roots_input_method_relay *relay = text_input->relay;
	if (!text_input->input->current_enabled) {
		wlr_log(WLR_INFO, "Inactive text input tried to commit an update");
		return;
	}
	wlr_log(WLR_DEBUG, "Text input committed update");
	if (relay->input_method == NULL) {
		wlr_log(WLR_INFO, "Text input committed, but input method is gone");
		return;
	}
	relay_send_im_done(relay, text_input->input);
}

static void relay_disable_text_input(struct roots_input_method_relay *relay,
		struct roots_text_input *text_input) {
	if (relay->input_method == NULL) {
		wlr_log(WLR_DEBUG, "Disabling text input, but input method is gone");
		return;
	}
	// relay_send_im_done protects from receiving unfocussed done,
	// but deactivate must be prevented too
	if (!text_input_is_focused(text_input->input)) {
		return;
	}
	wlr_input_method_v2_send_deactivate(relay->input_method);
	relay_send_im_done(relay, text_input->input);
}

static void handle_text_input_disable(struct wl_listener *listener,
		void *data) {
	struct roots_text_input *text_input = wl_container_of(listener, text_input,
		disable);
	struct roots_input_method_relay *relay = text_input->relay;
	relay_disable_text_input(relay, text_input);
}

static void handle_text_input_destroy(struct wl_listener *listener,
		void *data) {
	struct roots_text_input *text_input = wl_container_of(listener, text_input,
		destroy);
	struct roots_input_method_relay *relay = text_input->relay;

	if (text_input->input->current_enabled) {
		relay_disable_text_input(relay, text_input);
	}
	text_input_clear_pending_focused_surface(text_input);
	wl_list_remove(&text_input->commit.link);
	wl_list_remove(&text_input->destroy.link);
	wl_list_remove(&text_input->disable.link);
	wl_list_remove(&text_input->enable.link);
	wl_list_remove(&text_input->link);
	text_input->input = NULL;
	free(text_input);
}

static void handle_pending_focused_surface_destroy(struct wl_listener *listener,
		void *data) {
	struct roots_text_input *text_input = wl_container_of(listener, text_input,
		pending_focused_surface_destroy);
	struct wlr_surface *surface = data;
	assert(text_input->pending_focused_surface == surface);
	text_input_clear_pending_focused_surface(text_input);
}

struct roots_text_input *roots_text_input_create(
		struct roots_input_method_relay *relay,
		struct wlr_text_input_v3 *text_input) {
	struct roots_text_input *input = calloc(1, sizeof(struct roots_text_input));
	if (!input) {
		return NULL;
	}
	input->input = text_input;
	input->relay = relay;

	wl_signal_add(&text_input->events.enable, &input->enable);
	input->enable.notify = handle_text_input_enable;

	wl_signal_add(&text_input->events.commit, &input->commit);
	input->commit.notify = handle_text_input_commit;

	wl_signal_add(&text_input->events.disable, &input->disable);
	input->disable.notify = handle_text_input_disable;

	wl_signal_add(&text_input->events.destroy, &input->destroy);
	input->destroy.notify = handle_text_input_destroy;

	input->pending_focused_surface_destroy.notify =
		handle_pending_focused_surface_destroy;
	wl_list_init(&input->pending_focused_surface_destroy.link);
	return input;
}

static void relay_handle_text_input(struct wl_listener *listener,
		void *data) {
	struct roots_input_method_relay *relay = wl_container_of(listener, relay,
		text_input_new);
	struct wlr_text_input_v3 *wlr_text_input = data;
	if (relay->seat->seat != wlr_text_input->seat) {
		return;
	}

	struct roots_text_input *text_input = roots_text_input_create(relay,
		wlr_text_input);
	if (!text_input) {
		return;
	}
	wl_list_insert(&relay->text_inputs, &text_input->link);
}

static void relay_handle_input_method(struct wl_listener *listener,
		void *data) {
	struct roots_input_method_relay *relay = wl_container_of(listener, relay,
		input_method_new);
	struct wlr_input_method_v2 *input_method = data;
	if (relay->seat->seat != input_method->seat) {
		return;
	}

	if (relay->input_method != NULL) {
		wlr_log(WLR_INFO, "Attempted to connect second input method to a seat");
		wlr_input_method_v2_send_unavailable(input_method);
		return;
	}

	relay->input_method = input_method;
	wl_signal_add(&relay->input_method->events.commit,
		&relay->input_method_commit);
	relay->input_method_commit.notify = handle_im_commit;
	wl_signal_add(&relay->input_method->events.destroy,
		&relay->input_method_destroy);
	relay->input_method_destroy.notify = handle_im_destroy;

	struct roots_text_input *text_input = relay_get_focusable_text_input(relay);
	if (text_input) {
		wlr_text_input_v3_send_enter(text_input->input,
			text_input->pending_focused_surface);
		text_input_clear_pending_focused_surface(text_input);
	}
}

void roots_input_method_relay_init(PhocSeat *seat,
		struct roots_input_method_relay *relay) {
        PhocServer *server = phoc_server_get_default ();
	relay->seat = seat;
	wl_list_init(&relay->text_inputs);

	relay->text_input_new.notify = relay_handle_text_input;
	wl_signal_add(&server->desktop->text_input->events.text_input,
		&relay->text_input_new);

	relay->input_method_new.notify = relay_handle_input_method;
	wl_signal_add(
		&server->desktop->input_method->events.input_method,
		&relay->input_method_new);
}

void roots_input_method_relay_destroy(struct roots_input_method_relay *relay) {
	wl_list_remove(&relay->text_input_new.link);
	wl_list_remove(&relay->input_method_new.link);
}

void roots_input_method_relay_set_focus(struct roots_input_method_relay *relay,
		struct wlr_surface *surface) {
	struct roots_text_input *text_input;
	wl_list_for_each(text_input, &relay->text_inputs, link) {
		if (text_input->pending_focused_surface) {
			assert(text_input->input->focused_surface == NULL);
			if (surface != text_input->pending_focused_surface) {
				text_input_clear_pending_focused_surface(text_input);
			}
		} else if (text_input->input->focused_surface) {
			assert(text_input->pending_focused_surface == NULL);
			if (surface != text_input->input->focused_surface) {
				relay_disable_text_input(relay, text_input);
				wlr_text_input_v3_send_leave(text_input->input);
			}
		}

		if (surface
		    && wl_resource_get_client(text_input->input->resource)
		    == wl_resource_get_client(surface->resource)) {
			if (relay->input_method) {
				if (surface != text_input->input->focused_surface) {
					wlr_text_input_v3_send_enter(text_input->input, surface);
				}
			} else if (surface != text_input->pending_focused_surface) {
				text_input_set_pending_focused_surface(text_input, surface);
			}
		}
	}
}
