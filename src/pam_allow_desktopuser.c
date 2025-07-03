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

static bool is_session_type_wayland(pam_handle_t* pamh, sd_bus* bus,
		const char* session_path)
{
	char* value = NULL;
	int ret = sd_bus_get_property_string(bus, "org.freedesktop.login1",
			session_path, "org.freedesktop.login1.Session", "Type",
			NULL, &value);
	bool result = ret == 0 && value && strcmp(value, "wayland") == 0;
	free(value);
	return result;
}

static bool is_session_class_greeter(pam_handle_t* pamh, sd_bus* bus,
		const char* session_path)
{
	char* value = NULL;
	int ret = sd_bus_get_property_string(bus, "org.freedesktop.login1",
			session_path, "org.freedesktop.login1.Session", "Class",
			NULL, &value);
	bool result = ret == 0 && value && strcmp(value, "greeter") == 0;
	free(value);
	return result;
}

static bool is_session_active(pam_handle_t* pamh, sd_bus* bus,
		const char* session_path)
{
	int active = 0;
	int ret = sd_bus_get_property_trivial(bus, "org.freedesktop.login1",
			session_path, "org.freedesktop.login1.Session", "Active",
			NULL, 'b', &active);
	return ret == 0 && active != 0;
}

static bool have_conflicting_session(pam_handle_t* pamh, sd_bus* bus,
		const char* auth_username)
{
	sd_bus_error error = SD_BUS_ERROR_NULL;
	sd_bus_message* m = NULL;
	bool found = false;

	int ret = sd_bus_call_method(bus, "org.freedesktop.login1",
			"/org/freedesktop/login1",
			"org.freedesktop.login1.Manager", "ListSessions",
			&error, &m, "");
	if (ret < 0) {
		pam_syslog(pamh, LOG_ERR, "Failed to call ListSessions: %s",
				error.message);
		goto done;
	}

	ret = sd_bus_message_enter_container(m, 'a', "(susso)");
	if (ret < 0) {
		pam_syslog(pamh, LOG_ERR, "Failed to enter session array: %s",
				strerror(-ret));
		goto done;
	}

	for (;;) {
		if (sd_bus_message_peek_type(m, NULL, NULL) <= 0)
			break;

		const char* session_user_name;
		const char* session_path;
		if (sd_bus_message_read(m, "(susso)", NULL, NULL,
					&session_user_name, NULL,
					&session_path) < 0) {
			pam_syslog(pamh, LOG_WARNING, "Failed to read session info");
			break;
		}

		if (strcmp(session_user_name, auth_username) == 0)
			continue;

		if (!is_session_type_wayland(pamh, bus, session_path))
			continue;

		if (!is_session_active(pamh, bus, session_path))
			continue;

		if (is_session_class_greeter(pamh, bus, session_path))
			continue;

		pam_syslog(pamh, LOG_WARNING,
				"Login for user '%s' rejected; another user is already using the desktop.",
				auth_username);
		found = true;
		break;
	}

done:
	sd_bus_error_free(&error);
	sd_bus_message_unref(m);
	return found;
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

	bool conflict = have_conflicting_session(pamh, bus, auth_username);
	sd_bus_unref(bus);
	return conflict ? PAM_PERM_DENIED : PAM_SUCCESS;
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
