#!/bin/sh

WAYVNC_CONFIG_PATH=/etc/wayvnc

generate_rsa_key()
{
	echo "Generating RSA key..."
	KEY_FILE="$WAYVNC_CONFIG_PATH/rsa_key.pem"
	ssh-keygen -m pem -f "$KEY_FILE" -t rsa -N "" >/dev/null
	rm -f "$KEY_FILE.pub"
	chown root:vnc "$KEY_FILE"
	chmod 640 "$KEY_FILE"
	echo "Done"
}

generate_tls_creds()
{
	echo "Generating TLS Credentials..."
	KEY_FILE="$WAYVNC_CONFIG_PATH/tls_key.pem"
	CERT_FILE="$WAYVNC_CONFIG_PATH/tls_cert.pem"
	HOSTNAME=$(cat /etc/hostname)
	openssl req -x509 -newkey rsa:4096 -sha256 -days 3650 -nodes \
		-keyout "$KEY_FILE" -out "$CERT_FILE" -subj /CN=$HOSTNAME \
		-addext subjectAltName=DNS:localhost,DNS:$HOSTNAME,DNS:$HOSTNAME.local 2>/dev/null
	chown root:vnc "$KEY_FILE" "$CERT_FILE"
	chmod 640 "$KEY_FILE" "$CERT_FILE"
	echo "Done"
}

test -e "$WAYVNC_CONFIG_PATH" || mkdir -p "$WAYVNC_CONFIG_PATH"
test -e "$WAYVNC_CONFIG_PATH/rsa_key.pem" || generate_rsa_key
test -e "$WAYVNC_CONFIG_PATH/tls_key.pem" -a \
	-e "$WAYVNC_CONFIG_PATH/tls_cert.pem" || generate_tls_creds
