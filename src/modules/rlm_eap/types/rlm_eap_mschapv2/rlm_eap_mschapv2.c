/*
 * rlm_eap_mschapv2.c    Handles that are called from eap
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
 *   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 *
 * Copyright 2003,2006  The FreeRADIUS server project
 */

RCSID("$Id$")

#include <stdio.h>
#include <stdlib.h>

#include "eap_mschapv2.h"

#include <freeradius-devel/rad_assert.h>

typedef struct rlm_eap_mschapv2_t {
	bool with_ntdomain_hack;
	bool send_error;
	char const *identity;
	int auth_type_mschap;
} rlm_eap_mschapv2_t;

static CONF_PARSER module_config[] = {
	{ FR_CONF_OFFSET("with_ntdomain_hack", PW_TYPE_BOOLEAN, rlm_eap_mschapv2_t, with_ntdomain_hack), .dflt = "no" },

	{ FR_CONF_OFFSET("send_error", PW_TYPE_BOOLEAN, rlm_eap_mschapv2_t, send_error), .dflt = "no" },
	{ FR_CONF_OFFSET("identity", PW_TYPE_STRING, rlm_eap_mschapv2_t, identity) },
	CONF_PARSER_TERMINATOR
};


static void fix_mppe_keys(eap_session_t *eap_session, mschapv2_opaque_t *data)
{
	fr_pair_list_mcopy_by_num(data, &data->mppe_keys, &eap_session->request->reply->vps, VENDORPEC_MICROSOFT, 7,
				  TAG_ANY);
	fr_pair_list_mcopy_by_num(data, &data->mppe_keys, &eap_session->request->reply->vps, VENDORPEC_MICROSOFT, 8,
				  TAG_ANY);
	fr_pair_list_mcopy_by_num(data, &data->mppe_keys, &eap_session->request->reply->vps, VENDORPEC_MICROSOFT, 16,
				  TAG_ANY);
	fr_pair_list_mcopy_by_num(data, &data->mppe_keys, &eap_session->request->reply->vps, VENDORPEC_MICROSOFT, 17,
				  TAG_ANY);
}

/*
 *	Attach the module.
 */
static int mod_instantiate(CONF_SECTION *cs, void **instance)
{
	rlm_eap_mschapv2_t *inst;
	fr_dict_enum_t const *dv;

	*instance = inst = talloc_zero(cs, rlm_eap_mschapv2_t);
	if (!inst) return -1;

	/*
	 *	Parse the configuration attributes.
	 */
	if (cf_section_parse(cs, inst, module_config) < 0) {
		return -1;
	}

	if (inst->identity && (strlen(inst->identity) > 255)) {
		cf_log_err_cs(cs, "identity is too long");
		return -1;
	}

	if (!inst->identity) {
		inst->identity = talloc_asprintf(inst, "freeradius-%s", RADIUSD_VERSION_STRING);
	}

	dv = fr_dict_enum_by_name(NULL, fr_dict_attr_by_num(NULL, 0, PW_AUTH_TYPE), "MS-CHAP");
	if (!dv) dv = fr_dict_enum_by_name(NULL, fr_dict_attr_by_num(NULL, 0, PW_AUTH_TYPE), "MSCHAP");
	if (!dv) {
		cf_log_err_cs(cs, "Failed to find 'Auth-Type MS-CHAP' section.  Cannot authenticate users.");
		return -1;
	}
	inst->auth_type_mschap = dv->value;

	return 0;
}


/*
 *	Compose the response.
 */
static int eapmschapv2_compose(rlm_eap_mschapv2_t *inst, eap_session_t *eap_session, VALUE_PAIR *reply) CC_HINT(nonnull);
static int eapmschapv2_compose(rlm_eap_mschapv2_t *inst, eap_session_t *eap_session, VALUE_PAIR *reply)
{
	uint8_t			*ptr;
	int16_t			length;
	mschapv2_header_t	*hdr;
	eap_round_t		*eap_round = eap_session->this_round;
	REQUEST			*request = eap_session->request;

	eap_round->request->code = PW_EAP_REQUEST;
	eap_round->request->type.num = PW_EAP_MSCHAPV2;

	/*
	 *	Always called with vendor Microsoft
	 */
	switch (reply->da->attr) {
	case PW_MSCHAP_CHALLENGE:
		/*
		 *   0                   1                   2                   3
		 *   0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
		 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
		 *  |     Code      |   Identifier  |            Length             |
		 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
		 *  |     Type      |   OpCode      |  MS-CHAPv2-ID |  MS-Length...
		 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
		 *  |   MS-Length   |  Value-Size   |  Challenge...
		 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
		 *  |                             Challenge...
		 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
		 *  |                             Server Name...
		 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
		 */
		length = MSCHAPV2_HEADER_LEN + MSCHAPV2_CHALLENGE_LEN + strlen(inst->identity);
		eap_round->request->type.data = talloc_array(eap_round->request, uint8_t, length);

		/*
		 *	Allocate room for the EAP-MS-CHAPv2 data.
		 */
		if (!eap_round->request->type.data) {
			return 0;
		}
		eap_round->request->type.length = length;

		ptr = eap_round->request->type.data;
		hdr = (mschapv2_header_t *) ptr;

		hdr->opcode = PW_EAP_MSCHAPV2_CHALLENGE;
		hdr->mschapv2_id = eap_round->response->id + 1;
		length = htons(length);
		memcpy(hdr->ms_length, &length, sizeof(uint16_t));
		hdr->value_size = MSCHAPV2_CHALLENGE_LEN;

		ptr += MSCHAPV2_HEADER_LEN;

		/*
		 *	Copy the Challenge, success, or error over.
		 */
		memcpy(ptr, reply->vp_octets, reply->vp_length);

		memcpy((ptr + reply->vp_length), inst->identity, strlen(inst->identity));
		break;

	case PW_MSCHAP2_SUCCESS:
		/*
		 *   0                   1                   2                   3
		 *   0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
		 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
		 *  |     Code      |   Identifier  |            Length             |
		 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
		 *  |     Type      |   OpCode      |  MS-CHAPv2-ID |  MS-Length...
		 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
		 *  |   MS-Length   |                    Message...
		 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
		 */
		RDEBUG2("MSCHAP Success");
		length = 46;
		eap_round->request->type.data = talloc_array(eap_round->request, uint8_t, length);
		/*
		 *	Allocate room for the EAP-MS-CHAPv2 data.
		 */
		if (!eap_round->request->type.data) {
			return 0;
		}
		memset(eap_round->request->type.data, 0, length);
		eap_round->request->type.length = length;

		eap_round->request->type.data[0] = PW_EAP_MSCHAPV2_SUCCESS;
		eap_round->request->type.data[1] = eap_round->response->id;
		length = htons(length);
		memcpy((eap_round->request->type.data + 2), &length, sizeof(uint16_t));
		memcpy((eap_round->request->type.data + 4), reply->vp_strvalue + 1, 42);
		break;

	case PW_MSCHAP_ERROR:
		REDEBUG("MSCHAP Failure");
		length = 4 + reply->vp_length - 1;
		eap_round->request->type.data = talloc_array(eap_round->request, uint8_t, length);

		/*
		 *	Allocate room for the EAP-MS-CHAPv2 data.
		 */
		if (!eap_round->request->type.data) return 0;
		memset(eap_round->request->type.data, 0, length);
		eap_round->request->type.length = length;

		eap_round->request->type.data[0] = PW_EAP_MSCHAPV2_FAILURE;
		eap_round->request->type.data[1] = eap_round->response->id;
		length = htons(length);
		memcpy((eap_round->request->type.data + 2), &length, sizeof(uint16_t));
		/*
		 *	Copy the entire failure message.
		 */
		memcpy((eap_round->request->type.data + 4),
		       reply->vp_strvalue + 1, reply->vp_length - 1);
		break;

	default:
		RERROR("Internal sanity check failed");
		return 0;
	}

	return 1;
}


static int CC_HINT(nonnull) mod_process(void *instance, eap_session_t *eap_session);

/*
 *	Initiate the EAP-MSCHAPV2 session by sending a challenge to the peer.
 */
static int mod_session_init(void *instance, eap_session_t *eap_session)
{
	int			i;
	VALUE_PAIR		*auth_challenge;
	VALUE_PAIR		*peer_challenge;
	mschapv2_opaque_t	*data;
	REQUEST			*request = eap_session->request;
	uint8_t 		*p;
	bool			created_auth_challenge;

	if (!rad_cond_assert(instance)) return 0;

	auth_challenge = fr_pair_find_by_num(request->control, VENDORPEC_MICROSOFT, PW_MSCHAP_CHALLENGE, TAG_ANY);
	if (auth_challenge && (auth_challenge->vp_length != MSCHAPV2_CHALLENGE_LEN)) {
		RWDEBUG("control:MS-CHAP-Challenge is incorrect length.  Ignoring it.");
		auth_challenge = NULL;
	}

	peer_challenge = fr_pair_find_by_num(request->control, 0, PW_MS_CHAP_PEER_CHALLENGE, TAG_ANY);
	if (peer_challenge && (peer_challenge->vp_length != MSCHAPV2_CHALLENGE_LEN)) {
		RWDEBUG("control:MS-CHAP-Peer-Challenge is incorrect length.  Ignoring it.");
		peer_challenge = NULL;
	}

	if (auth_challenge) {
		created_auth_challenge = false;

		peer_challenge = fr_pair_find_by_num(request->control, 0, PW_MS_CHAP_PEER_CHALLENGE, TAG_ANY);
		if (peer_challenge && (peer_challenge->vp_length != MSCHAPV2_CHALLENGE_LEN)) {
			RWDEBUG("control:MS-CHAP-Peer-Challenge is incorrect length.  Ignoring it.");
			peer_challenge = NULL;
		}

	} else {
		created_auth_challenge = true;
		peer_challenge = NULL;

		auth_challenge = fr_pair_make(eap_session, NULL, "MS-CHAP-Challenge", NULL, T_OP_EQ);

		/*
		 *	Get a random challenge.
		 */
		p = talloc_array(auth_challenge, uint8_t, MSCHAPV2_CHALLENGE_LEN);
		for (i = 0; i < MSCHAPV2_CHALLENGE_LEN; i++) p[i] = fr_rand();
		fr_pair_value_memsteal(auth_challenge, p);
	}
	RDEBUG2("Issuing Challenge");

	/*
	 *	Keep track of the challenge.
	 */
	data = talloc_zero(eap_session, mschapv2_opaque_t);
	rad_assert(data != NULL);

	/*
	 *	We're at the stage where we're challenging the user.
	 */
	data->code = PW_EAP_MSCHAPV2_CHALLENGE;
	memcpy(data->auth_challenge, auth_challenge->vp_octets, MSCHAPV2_CHALLENGE_LEN);
	data->mppe_keys = NULL;
	data->reply = NULL;

	if (peer_challenge) {
		data->has_peer_challenge = true;
		memcpy(data->peer_challenge, peer_challenge->vp_octets, MSCHAPV2_CHALLENGE_LEN);
	}

	eap_session->opaque = data;

	/*
	 *	Compose the EAP-MSCHAPV2 packet out of the data structure,
	 *	and free it.
	 */
	eapmschapv2_compose(instance, eap_session, auth_challenge);
	if (created_auth_challenge) fr_pair_list_free(&auth_challenge);

#ifdef WITH_PROXY
	/*
	 *	The EAP session doesn't have enough information to
	 *	proxy the "inside EAP" protocol.  Disable EAP proxying.
	 */
	eap_session->request->options &= ~RAD_REQUEST_OPTION_PROXY_EAP;
#endif

	/*
	 *	We don't need to authorize the user at this point.
	 *
	 *	We also don't need to keep the challenge, as it's
	 *	stored in 'eap_session->this_round', which will be given back
	 *	to us...
	 */
	eap_session->process = mod_process;

	return 1;
}

#ifdef WITH_PROXY
/*
 *	Do post-proxy processing,
 *	0 = fail
 *	1 = OK.
 *
 *	Called from rlm_eap.c, eap_postproxy().
 */
static int CC_HINT(nonnull) mschap_postproxy(eap_session_t *eap_session, UNUSED void *tunnel_data)
{
	VALUE_PAIR *response = NULL;
	mschapv2_opaque_t *data;
	REQUEST *request = eap_session->request;

	data = talloc_get_type_abort(eap_session->opaque, mschapv2_opaque_t);
	rad_assert(request != NULL);

	RDEBUG2("Passing reply from proxy back into the tunnel %d", request->reply->code);

	/*
	 *	There is only a limited number of possibilities.
	 */
	switch (request->reply->code) {
	case PW_CODE_ACCESS_ACCEPT:
		RDEBUG2("Proxied authentication succeeded");

		/*
		 *	Move the attribute, so it doesn't go into
		 *	the reply.
		 */
		fr_pair_list_mcopy_by_num(data, &response, &request->reply->vps, VENDORPEC_MICROSOFT,
					  PW_MSCHAP2_SUCCESS, TAG_ANY);
		break;

	default:
	case PW_CODE_ACCESS_REJECT:
		REDEBUG("Proxied authentication was rejected");
		return 0;
	}

	/*
	 *	No response, die.
	 */
	if (!response) {
		REDEBUG("Proxied reply contained no MS-CHAP2-Success or MS-CHAP-Error");
		return 0;
	}

	/*
	 *	Done doing EAP proxy stuff.
	 */
	request->options &= ~RAD_REQUEST_OPTION_PROXY_EAP;
	if (!rad_cond_assert(eap_session->inst)) return 0;
	eapmschapv2_compose(eap_session->inst, eap_session, response);
	data->code = PW_EAP_MSCHAPV2_SUCCESS;

	/*
	 *	Delete MPPE keys & encryption policy
	 *
	 *	FIXME: Use intelligent names...
	 */
	fix_mppe_keys(eap_session, data);

	/*
	 *	Save any other attributes for re-use in the final
	 *	access-accept e.g. vlan, etc. This lets the PEAP
	 *	use_tunneled_reply code work
	 */
	data->reply = fr_pair_list_copy(data, request->reply->vps);

	/*
	 *	And we need to challenge the user, not ack/reject them,
	 *	so we re-write the ACK to a challenge.  Yuck.
	 */
	request->reply->code = PW_CODE_ACCESS_CHALLENGE;
	fr_pair_list_free(&response);

	return 1;
}
#endif

/*
 *	Authenticate a previously sent challenge.
 */
static int CC_HINT(nonnull) mod_process(void *arg, eap_session_t *eap_session)
{
	int rcode, ccode;
	uint8_t *p;
	size_t length;
	char *q;
	mschapv2_opaque_t *data;
	eap_round_t *eap_round = eap_session->this_round;
	VALUE_PAIR *auth_challenge, *response, *name;
	rlm_eap_mschapv2_t *inst = (rlm_eap_mschapv2_t *) arg;
	REQUEST *request = eap_session->request;

	if (!rad_cond_assert(eap_session->inst)) return 0;

	data = (mschapv2_opaque_t *) eap_session->opaque;

	/*
	 *	Sanity check the response.
	 */
	if (eap_round->response->length <= 5) {
		REDEBUG("corrupted data");
		return 0;
	}

	ccode = eap_round->response->type.data[0];

	switch (data->code) {
	case PW_EAP_MSCHAPV2_FAILURE:
		if (ccode == PW_EAP_MSCHAPV2_RESPONSE) {
			RDEBUG2("Authentication re-try from client after we sent a failure");
			break;
		}

		/*
		 * if we sent error 648 (password expired) to the client
		 * we might get an MSCHAP-CPW packet here; turn it into a
		 * regular MS-CHAP2-CPW packet and pass it to rlm_mschap
		 * (or proxy it, I guess)
		 */
		if (ccode == PW_EAP_MSCHAPV2_CHGPASSWD) {
			VALUE_PAIR *cpw;
			int mschap_id = eap_round->response->type.data[1];
			int copied = 0 ,seq = 1;

			RDEBUG2("Password change packet received");

			auth_challenge = pair_make_request("MS-CHAP-Challenge", NULL, T_OP_EQ);
			if (!auth_challenge) return 0;
			fr_pair_value_memcpy(auth_challenge, data->auth_challenge, MSCHAPV2_CHALLENGE_LEN);

			cpw = pair_make_request("MS-CHAP2-CPW", NULL, T_OP_EQ);
			p = talloc_array(cpw, uint8_t, 68);
			p[0] = 7;
			p[1] = mschap_id;
			memcpy(p + 2, eap_round->response->type.data + 520, 66);
			fr_pair_value_memsteal(cpw, p);

			/*
			 * break the encoded password into VPs (3 of them)
			 */
			while (copied < 516) {
				VALUE_PAIR *nt_enc;

				int to_copy = 516 - copied;
				if (to_copy > 243) to_copy = 243;

				nt_enc = pair_make_request("MS-CHAP-NT-Enc-PW", NULL, T_OP_ADD);
				p = talloc_array(nt_enc, uint8_t, 4 + to_copy);
				p[0] = 6;
				p[1] = mschap_id;
				p[2] = 0;
				p[3] = seq++;
				memcpy(p + 4, eap_round->response->type.data + 4 + copied, to_copy);
				fr_pair_value_memsteal(nt_enc, p);

				copied += to_copy;
			}

			RDEBUG2("Built change password packet");
			rdebug_pair_list(L_DBG_LVL_2, request, request->packet->vps, NULL);

			/*
			 * jump to "authentication"
			 */
			goto packet_ready;
		}

		/*
		 * we sent a failure and are expecting a failure back
		 */
		if (ccode != PW_EAP_MSCHAPV2_FAILURE) {
			REDEBUG("Sent FAILURE expecting FAILURE but got %d", ccode);
			return 0;
		}

failure:
		request->options &= ~RAD_REQUEST_OPTION_PROXY_EAP;
		eap_round->request->code = PW_EAP_FAILURE;
		return 1;

	case PW_EAP_MSCHAPV2_SUCCESS:
		/*
		 * we sent a success to the client; some clients send a
		 * success back as-per the RFC, some send an ACK. Permit
		 * both, I guess...
		 */

		switch (ccode) {
		case PW_EAP_MSCHAPV2_SUCCESS:
			eap_round->request->code = PW_EAP_SUCCESS;

			fr_pair_list_mcopy_by_num(request->reply, &request->reply->vps, &data->mppe_keys, 0, 0, TAG_ANY);
			/* FALL-THROUGH */

		case PW_EAP_MSCHAPV2_ACK:
#ifdef WITH_PROXY
			/*
			 *	It's a success.  Don't proxy it.
			 */
			request->options &= ~RAD_REQUEST_OPTION_PROXY_EAP;
#endif
			fr_pair_list_mcopy_by_num(request->reply, &request->reply->vps, &data->reply, 0, 0, TAG_ANY);
			return 1;
		}
		REDEBUG("Sent SUCCESS expecting SUCCESS (or ACK) but got %d", ccode);
		return 0;

	case PW_EAP_MSCHAPV2_CHALLENGE:
		if (ccode == PW_EAP_MSCHAPV2_FAILURE) goto failure;

		/*
		 * we sent a challenge, expecting a response
		 */
		if (ccode != PW_EAP_MSCHAPV2_RESPONSE) {
			REDEBUG("Sent CHALLENGE expecting RESPONSE but got %d", ccode);
			return 0;
		}
		/* authentication happens below */
		break;

	default:
		/* should never happen */
		REDEBUG("Unknown state %d", data->code);
		return 0;
	}


	/*
	 *	Ensure that we have at least enough data
	 *	to do the following checks.
	 *
	 *	EAP header (4), EAP type, MS-CHAP opcode,
	 *	MS-CHAP ident, MS-CHAP data length (2),
	 *	MS-CHAP value length.
	 */
	if (eap_round->response->length < (4 + 1 + 1 + 1 + 2 + 1)) {
		REDEBUG("Response is too short");
		return 0;
	}

	/*
	 *	The 'value_size' is the size of the response,
	 *	which is supposed to be the response (48
	 *	bytes) plus 1 byte of flags at the end.
	 */
	if (eap_round->response->type.data[4] != 49) {
		REDEBUG("Response is of incorrect length %d", eap_round->response->type.data[4]);
		return 0;
	}

	/*
	 *	The MS-Length field is 5 + value_size + length
	 *	of name, which is put after the response.
	 */
	length = (eap_round->response->type.data[2] << 8) | eap_round->response->type.data[3];
	if ((length < (5 + 49)) || (length > (256 + 5 + 49))) {
		REDEBUG("Response contains contradictory length %zu %d", length, 5 + 49);
		return 0;
	}

	/*
	 *	We now know that the user has sent us a response
	 *	to the challenge.  Let's try to authenticate it.
	 *
	 *	We do this by taking the challenge from 'data',
	 *	the response from the EAP packet, and creating VALUE_PAIR's
	 *	to pass to the 'mschap' module.  This is a little wonky,
	 *	but it works.
	 */
	auth_challenge = pair_make_request("MS-CHAP-Challenge", NULL, T_OP_EQ);
	if (!auth_challenge) return 0;
	fr_pair_value_memcpy(auth_challenge, data->auth_challenge, MSCHAPV2_CHALLENGE_LEN);

	response = pair_make_request("MS-CHAP2-Response", NULL, T_OP_EQ);
	if (!response) return 0;

	p = talloc_array(response, uint8_t, MSCHAPV2_RESPONSE_LEN);
	p[0] = eap_round->response->type.data[1];
	p[1] = eap_round->response->type.data[5 + MSCHAPV2_RESPONSE_LEN];
	memcpy(p + 2, &eap_round->response->type.data[5], MSCHAPV2_RESPONSE_LEN - 2);

	/*
	 *	If we're forcing a peer challenge, use it instead of
	 *	the challenge sent by the client.
	 */
	if (data->has_peer_challenge) {
		memcpy(p + 2, data->peer_challenge, MSCHAPV2_CHALLENGE_LEN);
	}

	fr_pair_value_memsteal(response, p);

	name = pair_make_request("MS-CHAP-User-Name", NULL, T_OP_EQ);
	if (!name) return 0;

	/*
	 *	MS-Length - MS-Value - 5.
	 */
	name->vp_length = length - 49 - 5;
	name->vp_strvalue = q = talloc_array(name, char, name->vp_length + 1);
	memcpy(q, &eap_round->response->type.data[4 + MSCHAPV2_RESPONSE_LEN], name->vp_length);
	q[name->vp_length] = '\0';

packet_ready:

#ifdef WITH_PROXY
	/*
	 *	If this options is set, then we do NOT authenticate the
	 *	user here.  Instead, now that we've added the MS-CHAP
	 *	attributes to the request, we STOP, and let the outer
	 *	tunnel code handle it.
	 *
	 *	This means that the outer tunnel code will DELETE the
	 *	EAP attributes, and proxy the MS-CHAP attributes to a
	 *	home server.
	 */
	if (request->options & RAD_REQUEST_OPTION_PROXY_EAP) {
		int			ret;
		char			*username = NULL;
		eap_tunnel_data_t	*tunnel;

		RDEBUG2("Cancelling authentication and letting it be proxied");

		/*
		 *	Set up the callbacks for the tunnel
		 */
		tunnel = talloc_zero(request, eap_tunnel_data_t);

		tunnel->tls_session = arg;
		tunnel->callback = mschap_postproxy;

		/*
		 *	Associate the callback with the request.
		 */
		ret = request_data_add(request, request->proxy, REQUEST_DATA_EAP_TUNNEL_CALLBACK,
				       tunnel, false, false, false);
		rad_cond_assert(ret == 0);

		/*
		 *	The State attribute is NOT supposed to
		 *	go into the proxied packet, it will confuse
		 *	other RADIUS servers, and they will discard
		 *	the request.
		 *
		 *	The PEAP module will take care of adding
		 *	the State attribute back, before passing
		 *	the eap_session & request back into the tunnel.
		 */
		fr_pair_delete_by_num(&request->packet->vps, 0, PW_STATE, TAG_ANY);

		/*
		 *	Fix the User-Name when proxying, to strip off
		 *	the NT Domain, if we're told to, and a User-Name
		 *	exists, and there's a \\, meaning an NT-Domain
		 *	in the user name, THEN discard the user name.
		 */
		if (inst->with_ntdomain_hack &&
		    ((auth_challenge = fr_pair_find_by_num(request->packet->vps, 0, PW_USER_NAME, TAG_ANY)) != NULL) &&
		    ((username = memchr(auth_challenge->vp_octets, '\\', auth_challenge->vp_length)) != NULL)) {
			/*
			 *	Wipe out the NT domain.
			 *
			 *	FIXME: Put it into MS-CHAP-Domain?
			 */
			username++; /* skip the \\ */
			fr_pair_value_strcpy(auth_challenge, username);
		}

		/*
		 *	Remember that in the post-proxy stage, we've got
		 *	to do the work below, AFTER the call to MS-CHAP
		 *	authentication...
		 */
		return 1;
	}
#endif

	/*
	 *	This is a wild & crazy hack.
	 */
	rcode = process_authenticate(inst->auth_type_mschap, request);

	/*
	 *	Delete MPPE keys & encryption policy.  We don't
	 *	want these here.
	 */
	fix_mppe_keys(eap_session, data);

	/*
	 *	Take the response from the mschap module, and
	 *	return success or failure, depending on the result.
	 */
	response = NULL;
	if (rcode == RLM_MODULE_OK) {
		fr_pair_list_mcopy_by_num(data, &response, &request->reply->vps, VENDORPEC_MICROSOFT,
					  PW_MSCHAP2_SUCCESS, TAG_ANY);
		data->code = PW_EAP_MSCHAPV2_SUCCESS;
	} else if (inst->send_error) {
		fr_pair_list_mcopy_by_num(data, &response, &request->reply->vps, VENDORPEC_MICROSOFT, PW_MSCHAP_ERROR,
					  TAG_ANY);
		if (response) {
			int n,err,retry;
			char buf[34];

			VERIFY_VP(response);

			RDEBUG2("MSCHAP-Error: %s", response->vp_strvalue);

			/*
			 *	Parse the new challenge out of the
			 *	MS-CHAP-Error, so that if the client
			 *	issues a re-try, we will know which
			 *	challenge value that they used.
			 */
			n = sscanf(response->vp_strvalue, "%*cE=%d R=%d C=%32s", &err, &retry, &buf[0]);
			if (n == 3) {
				RDEBUG2("Found new challenge from MS-CHAP-Error: err=%d retry=%d challenge=%s",
					err, retry, buf);
				fr_hex2bin(data->auth_challenge, 16, buf, strlen(buf));
			} else {
				RDEBUG2("Could not parse new challenge from MS-CHAP-Error: %d", n);
			}
		}
		data->code = PW_EAP_MSCHAPV2_FAILURE;
	} else {
		eap_round->request->code = PW_EAP_FAILURE;
		return 1;
	}

	/*
	 *	No response, die.
	 */
	if (!response) {
		REDEBUG("No MS-CHAP2-Success or MS-CHAP-Error was found");
		return 0;
	}

	/*
	 *	Compose the response (whatever it is),
	 *	and return it to the over-lying EAP module.
	 */
	eapmschapv2_compose(eap_session->inst, eap_session, response);
	fr_pair_list_free(&response);

	return 1;
}

/*
 *	The module name should be the only globally exported symbol.
 *	That is, everything else should be 'static'.
 */
extern rlm_eap_module_t rlm_eap_mschapv2;
rlm_eap_module_t rlm_eap_mschapv2 = {
	.name		= "eap_mschapv2",
	.instantiate	= mod_instantiate,	/* Create new submodule instance */
	.session_init	= mod_session_init,	/* Initialise a new EAP session */
	.process	= mod_process		/* Process next round of EAP method */
};
