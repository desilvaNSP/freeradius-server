/*
 * cb.c
 *
 * Version:     $Id$
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * Copyright 2001  hereUare Communications, Inc. <raghud@hereuare.com>
 */
#include "eap_tls.h"

#ifndef NO_OPENSSL

void cbtls_info(const SSL *s, int where, int ret)
{
	const char *str, *state;
	int w;

	w = where & ~SSL_ST_MASK;
	if (w & SSL_ST_CONNECT) str="    TLS_connect";
	else if (w & SSL_ST_ACCEPT) str="    TLS_accept";
	else str="    (other)";

	state = SSL_state_string_long(s);
	state = state ? state : "NULL";

	if (where & SSL_CB_LOOP) {
		if (debug_flag) radlog(L_INFO, "%s: %s\n", str, state);
	} else if (where & SSL_CB_HANDSHAKE_START) {
		if (debug_flag) radlog(L_INFO, "%s: %s\n", str, state);
	} else if (where & SSL_CB_HANDSHAKE_DONE) {
		radlog(L_INFO, "%s: %s\n", str, state);
	} else if (where & SSL_CB_ALERT) {
		str=(where & SSL_CB_READ)?"read":"write";
		radlog(L_ERR,"TLS Alert %s:%s:%s\n", str,
			SSL_alert_type_string_long(ret),
			SSL_alert_desc_string_long(ret));
	} else if (where & SSL_CB_EXIT) {
		if (ret == 0)
			radlog(L_ERR, "%s:failed in %s\n", str, state);
		else if (ret < 0)
			radlog(L_ERR, "%s:error in %s\n", str, state);
	}
}

/*
 *	Before trusting a certificate, you must make sure that the
 *	certificate is 'valid'. There are several steps that your
 *	application can take in determining if a certificate is
 *	valid. Commonly used steps are:
 *
 *	1.Verifying the certificate's signature, and verifying that
 *	the certificate has been issued by a trusted Certificate
 *	Authority.
 *
 *	2.Verifying that the certificate is valid for the present date
 *	(i.e. it is being presented within its validity dates).
 *
 *	3.Verifying that the certificate has not been revoked by its
 *	issuing Certificate Authority, by checking with respect to a
 *	Certificate Revocation List (CRL).
 *
 *	4.Verifying that the credentials presented by the certificate
 *	fulfill additional requirements specific to the application,
 *	such as with respect to access control lists or with respect
 *	to OCSP (Online Certificate Status Processing).
 *
 *	NOTE: This callback will be called multiple times based on the
 *	depth of the root certificate chain
 */
int cbtls_verify(int ok, X509_STORE_CTX *ctx)
{
	char subject[256]; /* Used for the subject name */
	char issuer[256]; /* Used for the issuer name */
	char buf[256];
	char cn_str[256];
	EAP_HANDLER *handler = NULL;
	X509 *client_cert;
	SSL *ssl;
	int err, depth;
	EAP_TLS_CONF *conf;
	int my_ok = ok;

	client_cert = X509_STORE_CTX_get_current_cert(ctx);
	err = X509_STORE_CTX_get_error(ctx);
	depth = X509_STORE_CTX_get_error_depth(ctx);

	if(!my_ok)
		radlog(L_ERR,"--> verify error:num=%d:%s\n",err,
			X509_verify_cert_error_string(err));
	/*
	 *	Catch too long Certificate chains
	 */

	/*
	 * Retrieve the pointer to the SSL of the connection currently treated
	 * and the application specific data stored into the SSL object.
	 */
	ssl = X509_STORE_CTX_get_ex_data(ctx, SSL_get_ex_data_X509_STORE_CTX_idx());
	handler = (EAP_HANDLER *)SSL_get_ex_data(ssl, 0);
	conf = (EAP_TLS_CONF *)SSL_get_ex_data(ssl, 1);

	/*
	 *	Get the Subject & Issuer
	 */
	subject[0] = issuer[0] = '\0';
	X509_NAME_oneline(X509_get_subject_name(client_cert), subject, 256);
	X509_NAME_oneline(X509_get_issuer_name(ctx->current_cert), issuer, 256);

	/*
	 *	Get the Common Name
	 */
	X509_NAME_get_text_by_NID(X509_get_subject_name(client_cert),
             NID_commonName, buf, 256);

	switch (ctx->error) {

	case X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT:
		radlog(L_ERR, "issuer= %s\n", issuer);
		break;
	case X509_V_ERR_CERT_NOT_YET_VALID:
	case X509_V_ERR_ERROR_IN_CERT_NOT_BEFORE_FIELD:
		radlog(L_ERR, "notBefore=");
#if 0
		ASN1_TIME_print(bio_err, X509_get_notBefore(ctx->current_cert));
#endif
		break;
	case X509_V_ERR_CERT_HAS_EXPIRED:
	case X509_V_ERR_ERROR_IN_CERT_NOT_AFTER_FIELD:
		radlog(L_ERR, "notAfter=");
#if 0
		ASN1_TIME_print(bio_err, X509_get_notAfter(ctx->current_cert));
#endif
		break;
	}

	/*
	 *	If we're at the actual client cert and the conf tells
	 *	us to, check the CN in the cert against the xlat'ed
	 *	value
	 */
	if (depth == 0 && conf->check_cert_cn != NULL) {
		if (!radius_xlat(cn_str, sizeof(cn_str), conf->check_cert_cn, handler->request, NULL)) {
			radlog(L_ERR, "rlm_eap_tls (%s): xlat failed.",
                               conf->check_cert_cn);
			/* if this fails, fail the verification */
			my_ok = 0;
		}
		DEBUG2("    rlm_eap_tls: checking certificate CN (%s) with xlat'ed value (%s)", buf, cn_str);
		if (strncmp(cn_str, buf, sizeof(buf)) != 0) {
			my_ok = 0;
			radlog(L_AUTH, "rlm_eap_tls: Certificate CN (%s) does not match specified value (%s)!", buf, cn_str);
		}
	}

	if (debug_flag > 0) {
		radlog(L_INFO, "chain-depth=%d, ", depth);
		/*
		  if (depth > 0) {
		  return ok;
		  }
		*/
		radlog(L_INFO, "error=%d", err);

		radlog(L_INFO, "--> User-Name = %s", handler->identity);
		radlog(L_INFO, "--> BUF-Name = %s", buf);
		radlog(L_INFO, "--> subject = %s", subject);
		radlog(L_INFO, "--> issuer  = %s", issuer);
		radlog(L_INFO, "--> verify return:%d", my_ok);
	}
	return my_ok;
}


/*
 *	Fill in our 'info' with TLS data.
 */
void cbtls_msg(int write_p, int msg_version, int content_type,
	       const void *buf, size_t len,
	       SSL *ssl UNUSED, void *arg)
{
	tls_session_t *state = (tls_session_t *)arg;

	state->info.origin = (unsigned char)write_p;
	state->info.content_type = (unsigned char)content_type;
	state->info.record_len = len;
	state->info.version = msg_version;
	state->info.initialized = 1;

	if (content_type == SSL3_RT_ALERT) {
		state->info.alert_level = ((const unsigned char*)buf)[0];
		state->info.alert_description = ((const unsigned char*)buf)[1];
		state->info.handshake_type = 0x00;

	} else if (content_type == SSL3_RT_HANDSHAKE) {
		state->info.handshake_type = ((const unsigned char*)buf)[0];
		state->info.alert_level = 0x00;
		state->info.alert_description = 0x00;
	}
	tls_session_information(state);
}

int cbtls_password(char *buf,
		   int num UNUSED,
		   int rwflag UNUSED,
		   void *userdata)
{
	strcpy(buf, (char *)userdata);
	return(strlen((char *)userdata));
}

RSA *cbtls_rsa(SSL *s UNUSED, int is_export UNUSED, int keylength)
{
	static RSA *rsa_tmp=NULL;

	if (rsa_tmp == NULL) {
		radlog(L_INFO, "Generating temp (%d bit) RSA key...", keylength);
		rsa_tmp=RSA_generate_key(keylength, RSA_F4, NULL, NULL);
	}
	return(rsa_tmp);
}

#endif /* !defined(NO_OPENSSL) */
