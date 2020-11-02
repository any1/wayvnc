#include "pam_auth.h"

static int pam_return_pwd(int num_msg, const struct pam_message** msgm,
                          struct pam_response** response, void* appdata_ptr)
{
	struct credentials* cred = appdata_ptr;
	struct pam_response* resp = calloc(sizeof(*response), num_msg);
	for (int i = 0; i < num_msg; i++) {
		resp[i].resp_retcode = PAM_SUCCESS;
		switch(msgm[i]->msg_style) {
		case PAM_TEXT_INFO:
		case PAM_ERROR_MSG:
			resp[i].resp = 0;
			break;
		case PAM_PROMPT_ECHO_ON:
			resp[i].resp = strdup(cred->user);
			break;
		case PAM_PROMPT_ECHO_OFF:
			resp[i].resp = strdup(cred->password);
			break;
		default:
			free(resp);
			return PAM_CONV_ERR;
		}
	}

	*response = resp;
	return PAM_SUCCESS;
}

bool pam_auth(const char* username, const char* password)
{
	struct credentials cred = { username, password };
	struct pam_conv conv = { &pam_return_pwd, &cred };
	int result;
	const char* service = "wayvnc";
	pam_handle_t* pamh;
	if ((result = pam_start(service, username, &conv, &pamh)) != PAM_SUCCESS) {
		log_error("ERROR: PAM start failed: %d\n", result);
		return false;
	}

	if ((result = pam_authenticate(pamh, PAM_SILENT|PAM_DISALLOW_NULL_AUTHTOK)) != PAM_SUCCESS) {
		log_error("PAM authenticate failed: %d\n", result);
		return false;
	}

	if ((result = pam_acct_mgmt(pamh, 0)) != PAM_SUCCESS) {
		log_error("PAM account management failed: %d\n", result);
		return false;
	}

	if ((result = pam_end(pamh, result)) != PAM_SUCCESS) {
		log_error("ERROR: PAM end failed: %d\n", result);
		return false;
	}
	return true;
}
