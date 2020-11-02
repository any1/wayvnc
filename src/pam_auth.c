#include "pam_auth.h"

#include <stdlib.h>
#include <string.h>
#include <security/pam_appl.h>

#include "logging.h"

struct credentials {
	const char* user;
	const char* password;
};

static int pam_return_pwd(int num_msg, const struct pam_message** msgm,
                          struct pam_response** response, void* appdata_ptr)
{
	struct credentials* cred = appdata_ptr;
	struct pam_response* resp = calloc(sizeof(*response), num_msg);
	for (int i = 0; i < num_msg; i++) {
		resp[i].resp_retcode = PAM_SUCCESS;
		switch(msgm[i]->msg_style) {
		case PAM_PROMPT_ECHO_OFF:
			resp[i].resp = strdup(cred->password);
			break;
		default:
			goto error;
		}
	}

	*response = resp;
	return PAM_SUCCESS;

error:
	for (int i = 0; i < num_msg; i++) {
		free(resp[i].resp);
	}
	free(resp);
	return PAM_CONV_ERR;
}

bool pam_auth(const char* username, const char* password)
{
	struct credentials cred = { username, password };
	struct pam_conv conv = { &pam_return_pwd, &cred };
	const char* service = "wayvnc";
	pam_handle_t* pamh;
	int result = pam_start(service, username, &conv, &pamh);
	if (result != PAM_SUCCESS) {
		log_error("ERROR: PAM start failed: %s\n", pam_strerror(pamh, result));
		return false;
	}

	result = pam_authenticate(pamh, PAM_SILENT|PAM_DISALLOW_NULL_AUTHTOK);
	if (result != PAM_SUCCESS) {
		log_error("PAM authenticate failed: %s\n", pam_strerror(pamh, result));
		goto error;
	}

	result = pam_acct_mgmt(pamh, 0);
	if (result != PAM_SUCCESS) {
		log_error("PAM account management failed: %s\n", pam_strerror(pamh, result));
		goto error;
	}

error:
	pam_end(pamh, result);
	return result == PAM_SUCCESS;
}
