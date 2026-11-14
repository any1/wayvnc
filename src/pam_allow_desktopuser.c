#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <syslog.h>
#include <pwd.h>

#include <systemd/sd-bus.h>

#define PAM_SM_ACCOUNT
#include <security/pam_modules.h>
#include <security/pam_ext.h>

struct session_info {
	const char* user_name;
	const char* seat_id;
	const char* path;
	char* type;
	char* session_class;
	bool is_active;
};

static bool read_session_string(pam_handle_t* pamh, sd_bus* bus,
		const char* session_path, const char* property, char** value)
{
	int ret = sd_bus_get_property_string(bus, "org.freedesktop.login1",
			session_path, "org.freedesktop.login1.Session", property,
			NULL, value);
	if (ret < 0)
		pam_syslog(pamh, LOG_ERR, "Failed to read %s of %s: %s",
				property, session_path, strerror(-ret));
	return ret >= 0;
}

static bool read_session_active(pam_handle_t* pamh, sd_bus* bus,
		const char* session_path, bool* is_active)
{
	int active = 0;
	int ret = sd_bus_get_property_trivial(bus, "org.freedesktop.login1",
			session_path, "org.freedesktop.login1.Session", "Active",
			NULL, 'b', &active);
	if (ret < 0) {
		pam_syslog(pamh, LOG_ERR, "Failed to read Active of %s: %s",
				session_path, strerror(-ret));
		return false;
	}
	*is_active = active != 0;
	return true;
}

static bool read_session_properties(pam_handle_t* pamh, sd_bus* bus,
		struct session_info* session)
{
	return read_session_string(pamh, bus, session->path, "Type",
				&session->type) &&
		read_session_string(pamh, bus, session->path, "Class",
				&session->session_class) &&
		read_session_active(pamh, bus, session->path,
				&session->is_active);
}

static void free_session_properties(struct session_info* session)
{
	free(session->type);
	free(session->session_class);
	session->type = NULL;
	session->session_class = NULL;
}

static bool is_session_of_other_user(const struct session_info* session,
		const char* auth_username)
{
	return strcmp(session->user_name, auth_username) != 0;
}

static bool is_session_on_seat(const struct session_info* session)
{
	return session->seat_id && session->seat_id[0] != '\0';
}

static bool is_session_type_wayland(const struct session_info* session)
{
	return session->type && strcmp(session->type, "wayland") == 0;
}

static bool is_session_class_greeter(const struct session_info* session)
{
	return session->session_class &&
		strcmp(session->session_class, "greeter") == 0;
}

static bool is_desktop_session(const struct session_info* session)
{
	return is_session_type_wayland(session) && session->is_active &&
		!is_session_class_greeter(session);
}

static bool list_sessions(pam_handle_t* pamh, sd_bus* bus,
		sd_bus_message** sessions)
{
	sd_bus_error error = SD_BUS_ERROR_NULL;
	int ret = sd_bus_call_method(bus, "org.freedesktop.login1",
			"/org/freedesktop/login1",
			"org.freedesktop.login1.Manager", "ListSessions",
			&error, sessions, "");
	if (ret < 0) {
		pam_syslog(pamh, LOG_ERR, "Failed to call ListSessions: %s",
				error.message);
		sd_bus_error_free(&error);
		return false;
	}

	ret = sd_bus_message_enter_container(*sessions, 'a', "(susso)");
	if (ret < 0) {
		pam_syslog(pamh, LOG_ERR, "Failed to enter session array: %s",
				strerror(-ret));
		*sessions = sd_bus_message_unref(*sessions);
		return false;
	}

	return true;
}

static int read_next_session(pam_handle_t* pamh, sd_bus_message* sessions,
		struct session_info* session)
{
	int ret = sd_bus_message_read(sessions, "(susso)", NULL, NULL,
			&session->user_name, &session->seat_id, &session->path);
	if (ret < 0)
		pam_syslog(pamh, LOG_ERR, "Failed to read session info: %s",
				strerror(-ret));
	return ret;
}

static bool check_session_conflict(pam_handle_t* pamh, sd_bus* bus,
		struct session_info* session, const char* auth_username,
		bool* has_conflict)
{
	if (!is_session_of_other_user(session, auth_username) ||
			!is_session_on_seat(session))
		return true;

	bool ok = read_session_properties(pamh, bus, session);
	if (ok)
		*has_conflict = is_desktop_session(session);
	free_session_properties(session);
	return ok;
}

static bool query_conflicting_session(pam_handle_t* pamh, sd_bus* bus,
		const char* auth_username, bool* has_conflict)
{
	sd_bus_message* sessions = NULL;
	if (!list_sessions(pamh, bus, &sessions))
		return false;

	struct session_info session = {};
	bool ok = true;
	int ret = 0;
	while (ok && !*has_conflict) {
		ret = read_next_session(pamh, sessions, &session);
		if (ret <= 0)
			break;
		ok = check_session_conflict(pamh, bus, &session,
				auth_username, has_conflict);
	}

	sd_bus_message_unref(sessions);
	return ok && ret >= 0;
}

PAM_EXTERN int pam_sm_authenticate(pam_handle_t* pamh, int flags, int argc,
		const char* argv[])
{
	const char* auth_username = NULL;
	if (pam_get_user(pamh, &auth_username, NULL) != PAM_SUCCESS || !auth_username) {
		pam_syslog(pamh, LOG_ERR, "Could not get username from PAM.");
		return PAM_SERVICE_ERR;
	}

	sd_bus* bus = NULL;
	int ret = sd_bus_open_system(&bus);
	if (ret < 0) {
		pam_syslog(pamh, LOG_ERR, "Failed to connect to system bus: %s",
				strerror(-ret));
		return PAM_SERVICE_ERR;
	}

	bool has_conflict = false;
	bool ok = query_conflicting_session(pamh, bus, auth_username,
			&has_conflict);
	sd_bus_unref(bus);

	if (!ok)
		return PAM_SERVICE_ERR;

	if (has_conflict) {
		pam_syslog(pamh, LOG_WARNING,
				"Login for user '%s' rejected; another user is already using the desktop.",
				auth_username);
		return PAM_PERM_DENIED;
	}

	return PAM_SUCCESS;
}

PAM_EXTERN int pam_sm_acct_mgmt(pam_handle_t* pamh, int flags, int argc,
		const char* argv[])
{
	return PAM_SUCCESS;
}

PAM_EXTERN int pam_sm_setcred(pam_handle_t* pamh, int flags, int argc,
		const char* argv[])
{
	return PAM_SUCCESS;
}
