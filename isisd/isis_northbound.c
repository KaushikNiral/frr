/*
 * Copyright (C) 2001,2002   Sampo Saaristo
 *                           Tampere University of Technology
 *                           Institute of Communications Engineering
 * Copyright (C) 2018        Volta Networks
 *                           Emanuele Di Pascale
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; see the file COPYING; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <zebra.h>
#include "northbound.h"
#include "libfrr.h"
#include "linklist.h"
#include "log.h"
#include "isisd/dict.h"
#include "isisd/isis_constants.h"
#include "isisd/isis_common.h"
#include "isisd/isis_flags.h"
#include "isisd/isis_circuit.h"
#include "isisd/isisd.h"
#include "isisd/isis_lsp.h"
#include "isisd/isis_pdu.h"
#include "isisd/isis_dynhn.h"
#include "isisd/isis_misc.h"
#include "isisd/isis_csm.h"
#include "isisd/isis_adjacency.h"
#include "isisd/isis_spf.h"
#include "isisd/isis_te.h"
#include "isisd/isis_memory.h"
#include "isisd/isis_mt.h"
#include "isisd/isis_cli.h"
#include "isisd/isis_redist.h"
#include "lib/spf_backoff.h"
#include "lib/lib_errors.h"
#include "lib/vrf.h"

/*
 * XPath: /frr-isisd:isis/instance
 */
static int isis_instance_create(enum nb_event event,
				const struct lyd_node *dnode,
				union nb_resource *resource)
{
	struct isis_area *area;
	const char *area_tag;

	if (event != NB_EV_APPLY)
		return NB_OK;

	area_tag = yang_dnode_get_string(dnode, "./area-tag");
	area = isis_area_lookup(area_tag);
	if (area)
		return NB_ERR_INCONSISTENCY;

	area = isis_area_create(area_tag);
	/* save area in dnode to avoid looking it up all the time */
	yang_dnode_set_entry(dnode, area);

	return NB_OK;
}

static int isis_instance_delete(enum nb_event event,
				const struct lyd_node *dnode)
{
	const char *area_tag;

	if (event != NB_EV_APPLY)
		return NB_OK;

	area_tag = yang_dnode_get_string(dnode, "./area-tag");
	isis_area_destroy(area_tag);

	return NB_OK;
}

/*
 * XPath: /frr-isisd:isis/instance/is-type
 */
static int isis_instance_is_type_modify(enum nb_event event,
					const struct lyd_node *dnode,
					union nb_resource *resource)
{
	struct isis_area *area;
	int type;

	if (event != NB_EV_APPLY)
		return NB_OK;

	area = yang_dnode_get_entry(dnode, true);
	type = yang_dnode_get_enum(dnode, NULL);
	isis_area_is_type_set(area, type);

	return NB_OK;
}

/*
 * XPath: /frr-isisd:isis/instance/area-address
 */
static int isis_instance_area_address_create(enum nb_event event,
					     const struct lyd_node *dnode,
					     union nb_resource *resource)
{
	struct isis_area *area;
	struct area_addr addr, *addrr = NULL, *addrp = NULL;
	struct listnode *node;
	uint8_t buff[255];
	const char *net_title = yang_dnode_get_string(dnode, NULL);

	switch (event) {
	case NB_EV_VALIDATE:
		addr.addr_len = dotformat2buff(buff, net_title);
		memcpy(addr.area_addr, buff, addr.addr_len);
		if (addr.area_addr[addr.addr_len - 1] != 0) {
			flog_warn(
				EC_LIB_NB_CB_CONFIG_VALIDATE,
				"nsel byte (last byte) in area address must be 0");
			return NB_ERR_VALIDATION;
		}
		if (isis->sysid_set) {
			/* Check that the SystemID portions match */
			if (memcmp(isis->sysid, GETSYSID((&addr)),
				   ISIS_SYS_ID_LEN)) {
				flog_warn(
					EC_LIB_NB_CB_CONFIG_VALIDATE,
					"System ID must not change when defining additional area addresses");
				return NB_ERR_VALIDATION;
			}
		}
		break;
	case NB_EV_PREPARE:
		addrr = XMALLOC(MTYPE_ISIS_AREA_ADDR, sizeof(struct area_addr));
		addrr->addr_len = dotformat2buff(buff, net_title);
		memcpy(addrr->area_addr, buff, addrr->addr_len);
		resource->ptr = addrr;
		break;
	case NB_EV_ABORT:
		XFREE(MTYPE_ISIS_AREA_ADDR, resource->ptr);
		break;
	case NB_EV_APPLY:
		area = yang_dnode_get_entry(dnode, true);
		addrr = resource->ptr;

		if (isis->sysid_set == 0) {
			/*
			 * First area address - get the SystemID for this router
			 */
			memcpy(isis->sysid, GETSYSID(addrr), ISIS_SYS_ID_LEN);
			isis->sysid_set = 1;
		} else {
			/* check that we don't already have this address */
			for (ALL_LIST_ELEMENTS_RO(area->area_addrs, node,
						  addrp)) {
				if ((addrp->addr_len + ISIS_SYS_ID_LEN
				     + ISIS_NSEL_LEN)
				    != (addrr->addr_len))
					continue;
				if (!memcmp(addrp->area_addr, addrr->area_addr,
					    addrr->addr_len)) {
					XFREE(MTYPE_ISIS_AREA_ADDR, addrr);
					return NB_OK; /* silent fail */
				}
			}
		}

		/*Forget the systemID part of the address */
		addrr->addr_len -= (ISIS_SYS_ID_LEN + ISIS_NSEL_LEN);
		assert(area->area_addrs); /* to silence scan-build sillyness */
		listnode_add(area->area_addrs, addrr);

		/* only now we can safely generate our LSPs for this area */
		if (listcount(area->area_addrs) > 0) {
			if (area->is_type & IS_LEVEL_1)
				lsp_generate(area, IS_LEVEL_1);
			if (area->is_type & IS_LEVEL_2)
				lsp_generate(area, IS_LEVEL_2);
		}
		break;
	}

	return NB_OK;
}

static int isis_instance_area_address_delete(enum nb_event event,
					     const struct lyd_node *dnode)
{
	struct area_addr addr, *addrp = NULL;
	struct listnode *node;
	uint8_t buff[255];
	struct isis_area *area;
	const char *net_title;

	if (event != NB_EV_APPLY)
		return NB_OK;

	net_title = yang_dnode_get_string(dnode, NULL);
	addr.addr_len = dotformat2buff(buff, net_title);
	memcpy(addr.area_addr, buff, (int)addr.addr_len);
	area = yang_dnode_get_entry(dnode, true);
	for (ALL_LIST_ELEMENTS_RO(area->area_addrs, node, addrp)) {
		if ((addrp->addr_len + ISIS_SYS_ID_LEN + 1) == addr.addr_len
		    && !memcmp(addrp->area_addr, addr.area_addr, addr.addr_len))
			break;
	}
	if (!addrp)
		return NB_ERR_INCONSISTENCY;

	listnode_delete(area->area_addrs, addrp);
	XFREE(MTYPE_ISIS_AREA_ADDR, addrp);
	/*
	 * Last area address - reset the SystemID for this router
	 */
	if (listcount(area->area_addrs) == 0) {
		memset(isis->sysid, 0, ISIS_SYS_ID_LEN);
		isis->sysid_set = 0;
		if (isis->debugs & DEBUG_EVENTS)
			zlog_debug("Router has no SystemID");
	}

	return NB_OK;
}

/*
 * XPath: /frr-isisd:isis/instance/dynamic-hostname
 */
static int isis_instance_dynamic_hostname_modify(enum nb_event event,
						 const struct lyd_node *dnode,
						 union nb_resource *resource)
{
	struct isis_area *area;

	if (event != NB_EV_APPLY)
		return NB_OK;

	area = yang_dnode_get_entry(dnode, true);
	isis_area_dynhostname_set(area, yang_dnode_get_bool(dnode, NULL));

	return NB_OK;
}

/*
 * XPath: /frr-isisd:isis/instance/attached
 */
static int isis_instance_attached_create(enum nb_event event,
					 const struct lyd_node *dnode,
					 union nb_resource *resource)
{
	struct isis_area *area;

	if (event != NB_EV_APPLY)
		return NB_OK;

	area = yang_dnode_get_entry(dnode, true);
	isis_area_attached_bit_set(area, true);

	return NB_OK;
}

static int isis_instance_attached_delete(enum nb_event event,
					 const struct lyd_node *dnode)
{
	struct isis_area *area;

	if (event != NB_EV_APPLY)
		return NB_OK;

	area = yang_dnode_get_entry(dnode, true);
	isis_area_attached_bit_set(area, false);

	return NB_OK;
}

/*
 * XPath: /frr-isisd:isis/instance/overload
 */
static int isis_instance_overload_create(enum nb_event event,
					 const struct lyd_node *dnode,
					 union nb_resource *resource)
{
	struct isis_area *area;

	if (event != NB_EV_APPLY)
		return NB_OK;

	area = yang_dnode_get_entry(dnode, true);
	isis_area_overload_bit_set(area, true);

	return NB_OK;
}

static int isis_instance_overload_delete(enum nb_event event,
					 const struct lyd_node *dnode)
{
	struct isis_area *area;

	if (event != NB_EV_APPLY)
		return NB_OK;

	area = yang_dnode_get_entry(dnode, true);
	isis_area_overload_bit_set(area, false);

	return NB_OK;
}

/*
 * XPath: /frr-isisd:isis/instance/metric-style
 */
static int isis_instance_metric_style_modify(enum nb_event event,
					     const struct lyd_node *dnode,
					     union nb_resource *resource)
{
	struct isis_area *area;
	bool old_metric, new_metric;
	enum isis_metric_style metric_style = yang_dnode_get_enum(dnode, NULL);

	if (event != NB_EV_APPLY)
		return NB_OK;

	area = yang_dnode_get_entry(dnode, true);
	old_metric = (metric_style == ISIS_WIDE_METRIC) ? false : true;
	new_metric = (metric_style == ISIS_NARROW_METRIC) ? false : true;
	isis_area_metricstyle_set(area, old_metric, new_metric);

	return NB_OK;
}

/*
 * XPath: /frr-isisd:isis/instance/purge-originator
 */
static int isis_instance_purge_originator_create(enum nb_event event,
						 const struct lyd_node *dnode,
						 union nb_resource *resource)
{
	/* TODO: implement me. */
	return NB_OK;
}

static int isis_instance_purge_originator_delete(enum nb_event event,
						 const struct lyd_node *dnode)
{
	/* TODO: implement me. */
	return NB_OK;
}

/*
 * XPath: /frr-isisd:isis/instance/lsp/mtu
 */
static int isis_instance_lsp_mtu_modify(enum nb_event event,
					const struct lyd_node *dnode,
					union nb_resource *resource)
{
	/* TODO: implement me. */
	return NB_OK;
}

/*
 * XPath: /frr-isisd:isis/instance/lsp/refresh-interval/level-1
 */
static int
isis_instance_lsp_refresh_interval_level_1_modify(enum nb_event event,
						  const struct lyd_node *dnode,
						  union nb_resource *resource)
{
	struct isis_area *area;
	uint16_t refr_int;

	if (event != NB_EV_APPLY)
		return NB_OK;

	refr_int = yang_dnode_get_uint16(dnode, NULL);
	area = yang_dnode_get_entry(dnode, true);
	isis_area_lsp_refresh_set(area, IS_LEVEL_1, refr_int);

	return NB_OK;
}

/*
 * XPath: /frr-isisd:isis/instance/lsp/refresh-interval/level-2
 */
static int
isis_instance_lsp_refresh_interval_level_2_modify(enum nb_event event,
						  const struct lyd_node *dnode,
						  union nb_resource *resource)
{
	struct isis_area *area;
	uint16_t refr_int;

	if (event != NB_EV_APPLY)
		return NB_OK;

	refr_int = yang_dnode_get_uint16(dnode, NULL);
	area = yang_dnode_get_entry(dnode, true);
	isis_area_lsp_refresh_set(area, IS_LEVEL_2, refr_int);

	return NB_OK;
}

/*
 * XPath: /frr-isisd:isis/instance/lsp/maximum-lifetime/level-1
 */
static int
isis_instance_lsp_maximum_lifetime_level_1_modify(enum nb_event event,
						  const struct lyd_node *dnode,
						  union nb_resource *resource)
{
	/* TODO: implement me. */
	return NB_OK;
}

/*
 * XPath: /frr-isisd:isis/instance/lsp/maximum-lifetime/level-2
 */
static int
isis_instance_lsp_maximum_lifetime_level_2_modify(enum nb_event event,
						  const struct lyd_node *dnode,
						  union nb_resource *resource)
{
	/* TODO: implement me. */
	return NB_OK;
}

/*
 * XPath: /frr-isisd:isis/instance/lsp/generation-interval/level-1
 */
static int isis_instance_lsp_generation_interval_level_1_modify(
	enum nb_event event, const struct lyd_node *dnode,
	union nb_resource *resource)
{
	struct isis_area *area;
	uint16_t gen_int;

	if (event != NB_EV_APPLY)
		return NB_OK;

	gen_int = yang_dnode_get_uint16(dnode, NULL);
	area = yang_dnode_get_entry(dnode, true);
	area->lsp_gen_interval[0] = gen_int;

	return NB_OK;
}

/*
 * XPath: /frr-isisd:isis/instance/lsp/generation-interval/level-2
 */
static int isis_instance_lsp_generation_interval_level_2_modify(
	enum nb_event event, const struct lyd_node *dnode,
	union nb_resource *resource)
{
	struct isis_area *area;
	uint16_t gen_int;

	if (event != NB_EV_APPLY)
		return NB_OK;

	gen_int = yang_dnode_get_uint16(dnode, NULL);
	area = yang_dnode_get_entry(dnode, true);
	area->lsp_gen_interval[1] = gen_int;

	return NB_OK;
}

/*
 * XPath: /frr-isisd:isis/instance/spf/ietf-backoff-delay
 */
static int
isis_instance_spf_ietf_backoff_delay_create(enum nb_event event,
					    const struct lyd_node *dnode,
					    union nb_resource *resource)
{
	/* TODO: implement me. */
	return NB_OK;
}

static int
isis_instance_spf_ietf_backoff_delay_delete(enum nb_event event,
					    const struct lyd_node *dnode)
{
	/* TODO: implement me. */
	return NB_OK;
}

/*
 * XPath: /frr-isisd:isis/instance/spf/ietf-backoff-delay/init-delay
 */
static int isis_instance_spf_ietf_backoff_delay_init_delay_modify(
	enum nb_event event, const struct lyd_node *dnode,
	union nb_resource *resource)
{
	/* TODO: implement me. */
	return NB_OK;
}

/*
 * XPath: /frr-isisd:isis/instance/spf/ietf-backoff-delay/short-delay
 */
static int isis_instance_spf_ietf_backoff_delay_short_delay_modify(
	enum nb_event event, const struct lyd_node *dnode,
	union nb_resource *resource)
{
	/* TODO: implement me. */
	return NB_OK;
}

/*
 * XPath: /frr-isisd:isis/instance/spf/ietf-backoff-delay/long-delay
 */
static int isis_instance_spf_ietf_backoff_delay_long_delay_modify(
	enum nb_event event, const struct lyd_node *dnode,
	union nb_resource *resource)
{
	/* TODO: implement me. */
	return NB_OK;
}

/*
 * XPath: /frr-isisd:isis/instance/spf/ietf-backoff-delay/hold-down
 */
static int isis_instance_spf_ietf_backoff_delay_hold_down_modify(
	enum nb_event event, const struct lyd_node *dnode,
	union nb_resource *resource)
{
	/* TODO: implement me. */
	return NB_OK;
}

/*
 * XPath: /frr-isisd:isis/instance/spf/ietf-backoff-delay/time-to-learn
 */
static int isis_instance_spf_ietf_backoff_delay_time_to_learn_modify(
	enum nb_event event, const struct lyd_node *dnode,
	union nb_resource *resource)
{
	/* TODO: implement me. */
	return NB_OK;
}

/*
 * XPath: /frr-isisd:isis/instance/spf/minimum-interval/level-1
 */
static int
isis_instance_spf_minimum_interval_level_1_modify(enum nb_event event,
						  const struct lyd_node *dnode,
						  union nb_resource *resource)
{
	/* TODO: implement me. */
	return NB_OK;
}

/*
 * XPath: /frr-isisd:isis/instance/spf/minimum-interval/level-2
 */
static int
isis_instance_spf_minimum_interval_level_2_modify(enum nb_event event,
						  const struct lyd_node *dnode,
						  union nb_resource *resource)
{
	/* TODO: implement me. */
	return NB_OK;
}

/*
 * XPath: /frr-isisd:isis/instance/area-password
 */
static void area_password_apply_finish(const struct lyd_node *dnode)
{
	const char *password = yang_dnode_get_string(dnode, "./password");
	struct isis_area *area = yang_dnode_get_entry(dnode, true);
	int pass_type = yang_dnode_get_enum(dnode, "./password-type");
	uint8_t snp_auth = yang_dnode_get_enum(dnode, "./authenticate-snp");

	switch (pass_type) {
	case ISIS_PASSWD_TYPE_CLEARTXT:
		isis_area_passwd_cleartext_set(area, IS_LEVEL_1, password,
					       snp_auth);
		break;
	case ISIS_PASSWD_TYPE_HMAC_MD5:
		isis_area_passwd_hmac_md5_set(area, IS_LEVEL_1, password,
					      snp_auth);
		break;
	}
}

static int isis_instance_area_password_create(enum nb_event event,
					      const struct lyd_node *dnode,
					      union nb_resource *resource)
{
	/* actual setting is done in apply_finish */
	return NB_OK;
}

static int isis_instance_area_password_delete(enum nb_event event,
					      const struct lyd_node *dnode)
{
	struct isis_area *area;

	if (event != NB_EV_APPLY)
		return NB_OK;

	area = yang_dnode_get_entry(dnode, true);
	isis_area_passwd_unset(area, IS_LEVEL_1);

	return NB_OK;
}

/*
 * XPath: /frr-isisd:isis/instance/area-password/password
 */
static int
isis_instance_area_password_password_modify(enum nb_event event,
					    const struct lyd_node *dnode,
					    union nb_resource *resource)
{
	/* actual setting is done in apply_finish */
	return NB_OK;
}

/*
 * XPath: /frr-isisd:isis/instance/area-password/password-type
 */
static int
isis_instance_area_password_password_type_modify(enum nb_event event,
						 const struct lyd_node *dnode,
						 union nb_resource *resource)
{
	/* actual setting is done in apply_finish */
	return NB_OK;
}

/*
 * XPath: /frr-isisd:isis/instance/area-password/authenticate-snp
 */
static int isis_instance_area_password_authenticate_snp_modify(
	enum nb_event event, const struct lyd_node *dnode,
	union nb_resource *resource)
{
	/* actual setting is done in apply_finish */
	return NB_OK;
}

/*
 * XPath: /frr-isisd:isis/instance/domain-password
 */
static void domain_password_apply_finish(const struct lyd_node *dnode)
{
	const char *password = yang_dnode_get_string(dnode, "./password");
	struct isis_area *area = yang_dnode_get_entry(dnode, true);
	int pass_type = yang_dnode_get_enum(dnode, "./password-type");
	uint8_t snp_auth = yang_dnode_get_enum(dnode, "./authenticate-snp");

	switch (pass_type) {
	case ISIS_PASSWD_TYPE_CLEARTXT:
		isis_area_passwd_cleartext_set(area, IS_LEVEL_2, password,
					       snp_auth);
		break;
	case ISIS_PASSWD_TYPE_HMAC_MD5:
		isis_area_passwd_hmac_md5_set(area, IS_LEVEL_2, password,
					      snp_auth);
		break;
	}
}

static int isis_instance_domain_password_create(enum nb_event event,
						const struct lyd_node *dnode,
						union nb_resource *resource)
{
	/* actual setting is done in apply_finish */
	return NB_OK;
}

static int isis_instance_domain_password_delete(enum nb_event event,
						const struct lyd_node *dnode)
{
	struct isis_area *area;

	if (event != NB_EV_APPLY)
		return NB_OK;

	area = yang_dnode_get_entry(dnode, true);
	isis_area_passwd_unset(area, IS_LEVEL_2);

	return NB_OK;
}

/*
 * XPath: /frr-isisd:isis/instance/domain-password/password
 */
static int
isis_instance_domain_password_password_modify(enum nb_event event,
					      const struct lyd_node *dnode,
					      union nb_resource *resource)
{
	/* actual setting is done in apply_finish */
	return NB_OK;
}

/*
 * XPath: /frr-isisd:isis/instance/domain-password/password-type
 */
static int
isis_instance_domain_password_password_type_modify(enum nb_event event,
						   const struct lyd_node *dnode,
						   union nb_resource *resource)
{
	/* actual setting is done in apply_finish */
	return NB_OK;
}

/*
 * XPath: /frr-isisd:isis/instance/domain-password/authenticate-snp
 */
static int isis_instance_domain_password_authenticate_snp_modify(
	enum nb_event event, const struct lyd_node *dnode,
	union nb_resource *resource)
{
	/* actual setting is done in apply_finish */
	return NB_OK;
}

/*
 * XPath: /frr-isisd:isis/instance/default-information-originate/ipv4
 */
static int isis_instance_default_information_originate_ipv4_create(
	enum nb_event event, const struct lyd_node *dnode,
	union nb_resource *resource)
{
	/* TODO: implement me. */
	return NB_OK;
}

static int isis_instance_default_information_originate_ipv4_delete(
	enum nb_event event, const struct lyd_node *dnode)
{
	/* TODO: implement me. */
	return NB_OK;
}

/*
 * XPath: /frr-isisd:isis/instance/default-information-originate/ipv4/always
 */
static int isis_instance_default_information_originate_ipv4_always_create(
	enum nb_event event, const struct lyd_node *dnode,
	union nb_resource *resource)
{
	/* TODO: implement me. */
	return NB_OK;
}

static int isis_instance_default_information_originate_ipv4_always_delete(
	enum nb_event event, const struct lyd_node *dnode)
{
	/* TODO: implement me. */
	return NB_OK;
}

/*
 * XPath: /frr-isisd:isis/instance/default-information-originate/ipv4/route-map
 */
static int isis_instance_default_information_originate_ipv4_route_map_modify(
	enum nb_event event, const struct lyd_node *dnode,
	union nb_resource *resource)
{
	/* TODO: implement me. */
	return NB_OK;
}

static int isis_instance_default_information_originate_ipv4_route_map_delete(
	enum nb_event event, const struct lyd_node *dnode)
{
	/* TODO: implement me. */
	return NB_OK;
}

/*
 * XPath: /frr-isisd:isis/instance/default-information-originate/ipv4/metric
 */
static int isis_instance_default_information_originate_ipv4_metric_modify(
	enum nb_event event, const struct lyd_node *dnode,
	union nb_resource *resource)
{
	/* TODO: implement me. */
	return NB_OK;
}

static int isis_instance_default_information_originate_ipv4_metric_delete(
	enum nb_event event, const struct lyd_node *dnode)
{
	/* TODO: implement me. */
	return NB_OK;
}

/*
 * XPath: /frr-isisd:isis/instance/default-information-originate/ipv6
 */
static int isis_instance_default_information_originate_ipv6_create(
	enum nb_event event, const struct lyd_node *dnode,
	union nb_resource *resource)
{
	/* TODO: implement me. */
	return NB_OK;
}

static int isis_instance_default_information_originate_ipv6_delete(
	enum nb_event event, const struct lyd_node *dnode)
{
	/* TODO: implement me. */
	return NB_OK;
}

/*
 * XPath: /frr-isisd:isis/instance/default-information-originate/ipv6/always
 */
static int isis_instance_default_information_originate_ipv6_always_create(
	enum nb_event event, const struct lyd_node *dnode,
	union nb_resource *resource)
{
	/* TODO: implement me. */
	return NB_OK;
}

static int isis_instance_default_information_originate_ipv6_always_delete(
	enum nb_event event, const struct lyd_node *dnode)
{
	/* TODO: implement me. */
	return NB_OK;
}

/*
 * XPath: /frr-isisd:isis/instance/default-information-originate/ipv6/route-map
 */
static int isis_instance_default_information_originate_ipv6_route_map_modify(
	enum nb_event event, const struct lyd_node *dnode,
	union nb_resource *resource)
{
	/* TODO: implement me. */
	return NB_OK;
}

static int isis_instance_default_information_originate_ipv6_route_map_delete(
	enum nb_event event, const struct lyd_node *dnode)
{
	/* TODO: implement me. */
	return NB_OK;
}

/*
 * XPath: /frr-isisd:isis/instance/default-information-originate/ipv6/metric
 */
static int isis_instance_default_information_originate_ipv6_metric_modify(
	enum nb_event event, const struct lyd_node *dnode,
	union nb_resource *resource)
{
	/* TODO: implement me. */
	return NB_OK;
}

static int isis_instance_default_information_originate_ipv6_metric_delete(
	enum nb_event event, const struct lyd_node *dnode)
{
	/* TODO: implement me. */
	return NB_OK;
}

/*
 * XPath: /frr-isisd:isis/instance/redistribute/ipv4
 */
static int isis_instance_redistribute_ipv4_create(enum nb_event event,
						  const struct lyd_node *dnode,
						  union nb_resource *resource)
{
	/* TODO: implement me. */
	return NB_OK;
}

static int isis_instance_redistribute_ipv4_delete(enum nb_event event,
						  const struct lyd_node *dnode)
{
	/* TODO: implement me. */
	return NB_OK;
}

/*
 * XPath: /frr-isisd:isis/instance/redistribute/ipv4/route-map
 */
static int
isis_instance_redistribute_ipv4_route_map_modify(enum nb_event event,
						 const struct lyd_node *dnode,
						 union nb_resource *resource)
{
	/* TODO: implement me. */
	return NB_OK;
}

static int
isis_instance_redistribute_ipv4_route_map_delete(enum nb_event event,
						 const struct lyd_node *dnode)
{
	/* TODO: implement me. */
	return NB_OK;
}

/*
 * XPath: /frr-isisd:isis/instance/redistribute/ipv4/metric
 */
static int
isis_instance_redistribute_ipv4_metric_modify(enum nb_event event,
					      const struct lyd_node *dnode,
					      union nb_resource *resource)
{
	/* TODO: implement me. */
	return NB_OK;
}

static int
isis_instance_redistribute_ipv4_metric_delete(enum nb_event event,
					      const struct lyd_node *dnode)
{
	/* TODO: implement me. */
	return NB_OK;
}

/*
 * XPath: /frr-isisd:isis/instance/redistribute/ipv6
 */
static int isis_instance_redistribute_ipv6_create(enum nb_event event,
						  const struct lyd_node *dnode,
						  union nb_resource *resource)
{
	/* TODO: implement me. */
	return NB_OK;
}

static int isis_instance_redistribute_ipv6_delete(enum nb_event event,
						  const struct lyd_node *dnode)
{
	/* TODO: implement me. */
	return NB_OK;
}

/*
 * XPath: /frr-isisd:isis/instance/redistribute/ipv6/route-map
 */
static int
isis_instance_redistribute_ipv6_route_map_modify(enum nb_event event,
						 const struct lyd_node *dnode,
						 union nb_resource *resource)
{
	/* TODO: implement me. */
	return NB_OK;
}

static int
isis_instance_redistribute_ipv6_route_map_delete(enum nb_event event,
						 const struct lyd_node *dnode)
{
	/* TODO: implement me. */
	return NB_OK;
}

/*
 * XPath: /frr-isisd:isis/instance/redistribute/ipv6/metric
 */
static int
isis_instance_redistribute_ipv6_metric_modify(enum nb_event event,
					      const struct lyd_node *dnode,
					      union nb_resource *resource)
{
	/* TODO: implement me. */
	return NB_OK;
}

static int
isis_instance_redistribute_ipv6_metric_delete(enum nb_event event,
					      const struct lyd_node *dnode)
{
	/* TODO: implement me. */
	return NB_OK;
}

/*
 * XPath: /frr-isisd:isis/instance/multi-topology/ipv4-multicast
 */
static int
isis_instance_multi_topology_ipv4_multicast_create(enum nb_event event,
						   const struct lyd_node *dnode,
						   union nb_resource *resource)
{
	/* TODO: implement me. */
	return NB_OK;
}

static int
isis_instance_multi_topology_ipv4_multicast_delete(enum nb_event event,
						   const struct lyd_node *dnode)
{
	/* TODO: implement me. */
	return NB_OK;
}

/*
 * XPath: /frr-isisd:isis/instance/multi-topology/ipv4-multicast/overload
 */
static int isis_instance_multi_topology_ipv4_multicast_overload_create(
	enum nb_event event, const struct lyd_node *dnode,
	union nb_resource *resource)
{
	/* TODO: implement me. */
	return NB_OK;
}

static int isis_instance_multi_topology_ipv4_multicast_overload_delete(
	enum nb_event event, const struct lyd_node *dnode)
{
	/* TODO: implement me. */
	return NB_OK;
}

/*
 * XPath: /frr-isisd:isis/instance/multi-topology/ipv4-management
 */
static int isis_instance_multi_topology_ipv4_management_create(
	enum nb_event event, const struct lyd_node *dnode,
	union nb_resource *resource)
{
	/* TODO: implement me. */
	return NB_OK;
}

static int isis_instance_multi_topology_ipv4_management_delete(
	enum nb_event event, const struct lyd_node *dnode)
{
	/* TODO: implement me. */
	return NB_OK;
}

/*
 * XPath: /frr-isisd:isis/instance/multi-topology/ipv4-management/overload
 */
static int isis_instance_multi_topology_ipv4_management_overload_create(
	enum nb_event event, const struct lyd_node *dnode,
	union nb_resource *resource)
{
	/* TODO: implement me. */
	return NB_OK;
}

static int isis_instance_multi_topology_ipv4_management_overload_delete(
	enum nb_event event, const struct lyd_node *dnode)
{
	/* TODO: implement me. */
	return NB_OK;
}

/*
 * XPath: /frr-isisd:isis/instance/multi-topology/ipv6-unicast
 */
static int
isis_instance_multi_topology_ipv6_unicast_create(enum nb_event event,
						 const struct lyd_node *dnode,
						 union nb_resource *resource)
{
	/* TODO: implement me. */
	return NB_OK;
}

static int
isis_instance_multi_topology_ipv6_unicast_delete(enum nb_event event,
						 const struct lyd_node *dnode)
{
	/* TODO: implement me. */
	return NB_OK;
}

/*
 * XPath: /frr-isisd:isis/instance/multi-topology/ipv6-unicast/overload
 */
static int isis_instance_multi_topology_ipv6_unicast_overload_create(
	enum nb_event event, const struct lyd_node *dnode,
	union nb_resource *resource)
{
	/* TODO: implement me. */
	return NB_OK;
}

static int isis_instance_multi_topology_ipv6_unicast_overload_delete(
	enum nb_event event, const struct lyd_node *dnode)
{
	/* TODO: implement me. */
	return NB_OK;
}

/*
 * XPath: /frr-isisd:isis/instance/multi-topology/ipv6-multicast
 */
static int
isis_instance_multi_topology_ipv6_multicast_create(enum nb_event event,
						   const struct lyd_node *dnode,
						   union nb_resource *resource)
{
	/* TODO: implement me. */
	return NB_OK;
}

static int
isis_instance_multi_topology_ipv6_multicast_delete(enum nb_event event,
						   const struct lyd_node *dnode)
{
	/* TODO: implement me. */
	return NB_OK;
}

/*
 * XPath: /frr-isisd:isis/instance/multi-topology/ipv6-multicast/overload
 */
static int isis_instance_multi_topology_ipv6_multicast_overload_create(
	enum nb_event event, const struct lyd_node *dnode,
	union nb_resource *resource)
{
	/* TODO: implement me. */
	return NB_OK;
}

static int isis_instance_multi_topology_ipv6_multicast_overload_delete(
	enum nb_event event, const struct lyd_node *dnode)
{
	/* TODO: implement me. */
	return NB_OK;
}

/*
 * XPath: /frr-isisd:isis/instance/multi-topology/ipv6-management
 */
static int isis_instance_multi_topology_ipv6_management_create(
	enum nb_event event, const struct lyd_node *dnode,
	union nb_resource *resource)
{
	/* TODO: implement me. */
	return NB_OK;
}

static int isis_instance_multi_topology_ipv6_management_delete(
	enum nb_event event, const struct lyd_node *dnode)
{
	/* TODO: implement me. */
	return NB_OK;
}

/*
 * XPath: /frr-isisd:isis/instance/multi-topology/ipv6-management/overload
 */
static int isis_instance_multi_topology_ipv6_management_overload_create(
	enum nb_event event, const struct lyd_node *dnode,
	union nb_resource *resource)
{
	/* TODO: implement me. */
	return NB_OK;
}

static int isis_instance_multi_topology_ipv6_management_overload_delete(
	enum nb_event event, const struct lyd_node *dnode)
{
	/* TODO: implement me. */
	return NB_OK;
}

/*
 * XPath: /frr-isisd:isis/instance/multi-topology/ipv6-dstsrc
 */
static int
isis_instance_multi_topology_ipv6_dstsrc_create(enum nb_event event,
						const struct lyd_node *dnode,
						union nb_resource *resource)
{
	/* TODO: implement me. */
	return NB_OK;
}

static int
isis_instance_multi_topology_ipv6_dstsrc_delete(enum nb_event event,
						const struct lyd_node *dnode)
{
	/* TODO: implement me. */
	return NB_OK;
}

/*
 * XPath: /frr-isisd:isis/instance/multi-topology/ipv6-dstsrc/overload
 */
static int isis_instance_multi_topology_ipv6_dstsrc_overload_create(
	enum nb_event event, const struct lyd_node *dnode,
	union nb_resource *resource)
{
	/* TODO: implement me. */
	return NB_OK;
}

static int isis_instance_multi_topology_ipv6_dstsrc_overload_delete(
	enum nb_event event, const struct lyd_node *dnode)
{
	/* TODO: implement me. */
	return NB_OK;
}

/*
 * XPath: /frr-isisd:isis/instance/log-adjacency-changes
 */
static int
isis_instance_log_adjacency_changes_create(enum nb_event event,
					   const struct lyd_node *dnode,
					   union nb_resource *resource)
{
	/* TODO: implement me. */
	return NB_OK;
}

static int
isis_instance_log_adjacency_changes_delete(enum nb_event event,
					   const struct lyd_node *dnode)
{
	/* TODO: implement me. */
	return NB_OK;
}

/*
 * XPath: /frr-isisd:isis/mpls-te
 */
static int isis_mpls_te_create(enum nb_event event,
			       const struct lyd_node *dnode,
			       union nb_resource *resource)
{
	/* TODO: implement me. */
	return NB_OK;
}

static int isis_mpls_te_delete(enum nb_event event,
			       const struct lyd_node *dnode)
{
	/* TODO: implement me. */
	return NB_OK;
}

/*
 * XPath: /frr-isisd:isis/mpls-te/router-address
 */
static int isis_mpls_te_router_address_modify(enum nb_event event,
					      const struct lyd_node *dnode,
					      union nb_resource *resource)
{
	/* TODO: implement me. */
	return NB_OK;
}

static int isis_mpls_te_router_address_delete(enum nb_event event,
					      const struct lyd_node *dnode)
{
	/* TODO: implement me. */
	return NB_OK;
}

/*
 * XPath: /frr-interface:lib/interface/frr-isisd:isis
 */
static int lib_interface_isis_create(enum nb_event event,
				     const struct lyd_node *dnode,
				     union nb_resource *resource)
{
	struct isis_area *area;
	struct interface *ifp;
	struct isis_circuit *circuit;
	const char *area_tag = yang_dnode_get_string(dnode, "./area-tag");

	if (event != NB_EV_APPLY)
		return NB_OK;

	area = isis_area_lookup(area_tag);
	/* The area should have already be created. We are
	 * setting the priority of the global isis area creation
	 * slightly lower, so it should be executed first, but I
	 * cannot rely on that so here I have to check.
	 */
	if (!area) {
		flog_err(
			EC_LIB_NB_CB_CONFIG_APPLY,
			"%s: attempt to create circuit for area %s before the area has been created",
			__func__, area_tag);
		abort();
	}

	ifp = yang_dnode_get_entry(dnode, true);
	circuit = isis_circuit_create(area, ifp);
	assert(circuit->state == C_STATE_CONF || circuit->state == C_STATE_UP);
	yang_dnode_set_entry(dnode, circuit);

	return NB_OK;
}

static int lib_interface_isis_delete(enum nb_event event,
				     const struct lyd_node *dnode)
{
	struct isis_circuit *circuit;

	if (event != NB_EV_APPLY)
		return NB_OK;

	circuit = yang_dnode_get_entry(dnode, true);
	if (!circuit)
		return NB_ERR_INCONSISTENCY;
	/* delete circuit through csm changes */
	switch (circuit->state) {
	case C_STATE_UP:
		isis_csm_state_change(IF_DOWN_FROM_Z, circuit,
				      circuit->interface);
		isis_csm_state_change(ISIS_DISABLE, circuit, circuit->area);
		break;
	case C_STATE_CONF:
		isis_csm_state_change(ISIS_DISABLE, circuit, circuit->area);
		break;
	case C_STATE_INIT:
		isis_csm_state_change(IF_DOWN_FROM_Z, circuit,
				      circuit->interface);
		break;
	}

	return NB_OK;
}

/*
 * XPath: /frr-interface:lib/interface/frr-isisd:isis/area-tag
 */
static int lib_interface_isis_area_tag_modify(enum nb_event event,
					      const struct lyd_node *dnode,
					      union nb_resource *resource)
{
	struct isis_circuit *circuit;
	struct interface *ifp;
	struct vrf *vrf;
	const char *area_tag, *ifname, *vrfname;

	if (event == NB_EV_VALIDATE) {
		/* libyang doesn't like relative paths across module boundaries
		 */
		ifname = yang_dnode_get_string(dnode->parent->parent, "./name");
		vrfname = yang_dnode_get_string(dnode->parent->parent, "./vrf");
		vrf = vrf_lookup_by_name(vrfname);
		assert(vrf);
		ifp = if_lookup_by_name(ifname, vrf->vrf_id);
		if (!ifp)
			return NB_OK;
		circuit = circuit_lookup_by_ifp(ifp, isis->init_circ_list);
		area_tag = yang_dnode_get_string(dnode, NULL);
		if (circuit && circuit->area && circuit->area->area_tag
		    && strcmp(circuit->area->area_tag, area_tag)) {
			flog_warn(EC_LIB_NB_CB_CONFIG_VALIDATE,
				  "ISIS circuit is already defined on %s",
				  circuit->area->area_tag);
			return NB_ERR_VALIDATION;
		}
	}

	return NB_OK;
}

/*
 * XPath: /frr-interface:lib/interface/frr-isisd:isis/circuit-type
 */
static int lib_interface_isis_circuit_type_modify(enum nb_event event,
						  const struct lyd_node *dnode,
						  union nb_resource *resource)
{
	int circ_type = yang_dnode_get_enum(dnode, NULL);
	struct isis_circuit *circuit;
	struct interface *ifp;
	struct vrf *vrf;
	const char *ifname, *vrfname;

	switch (event) {
	case NB_EV_VALIDATE:
		/* libyang doesn't like relative paths across module boundaries
		 */
		ifname = yang_dnode_get_string(dnode->parent->parent, "./name");
		vrfname = yang_dnode_get_string(dnode->parent->parent, "./vrf");
		vrf = vrf_lookup_by_name(vrfname);
		assert(vrf);
		ifp = if_lookup_by_name(ifname, vrf->vrf_id);
		if (!ifp)
			break;
		circuit = circuit_lookup_by_ifp(ifp, isis->init_circ_list);
		if (circuit && circuit->state == C_STATE_UP
		    && circuit->area->is_type != IS_LEVEL_1_AND_2
		    && circuit->area->is_type != circ_type) {
			flog_warn(EC_LIB_NB_CB_CONFIG_VALIDATE,
				  "Invalid circuit level for area %s",
				  circuit->area->area_tag);
			return NB_ERR_VALIDATION;
		}
		break;
	case NB_EV_PREPARE:
	case NB_EV_ABORT:
		break;
	case NB_EV_APPLY:
		circuit = yang_dnode_get_entry(dnode, true);
		isis_circuit_is_type_set(circuit, circ_type);
		break;
	}

	return NB_OK;
}

/*
 * XPath: /frr-interface:lib/interface/frr-isisd:isis/ipv4-routing
 */
static int lib_interface_isis_ipv4_routing_create(enum nb_event event,
						  const struct lyd_node *dnode,
						  union nb_resource *resource)
{
	bool ipv6;
	struct isis_circuit *circuit;

	if (event != NB_EV_APPLY)
		return NB_OK;

	circuit = yang_dnode_get_entry(dnode, true);
	ipv6 = yang_dnode_exists(dnode, "../ipv6-routing");
	isis_circuit_af_set(circuit, true, ipv6);

	return NB_OK;
}

static int lib_interface_isis_ipv4_routing_delete(enum nb_event event,
						  const struct lyd_node *dnode)
{
	bool ipv6;
	struct isis_circuit *circuit;

	if (event != NB_EV_APPLY)
		return NB_OK;

	circuit = yang_dnode_get_entry(dnode, true);
	if (circuit && circuit->area) {
		ipv6 = yang_dnode_exists(dnode, "../ipv6-routing");
		isis_circuit_af_set(circuit, false, ipv6);
	}

	return NB_OK;
}

/*
 * XPath: /frr-interface:lib/interface/frr-isisd:isis/ipv6-routing
 */
static int lib_interface_isis_ipv6_routing_create(enum nb_event event,
						  const struct lyd_node *dnode,
						  union nb_resource *resource)
{
	bool ipv4;
	struct isis_circuit *circuit;

	if (event != NB_EV_APPLY)
		return NB_OK;

	circuit = yang_dnode_get_entry(dnode, true);
	ipv4 = yang_dnode_exists(dnode, "../ipv6-routing");
	isis_circuit_af_set(circuit, ipv4, true);

	return NB_OK;
}

static int lib_interface_isis_ipv6_routing_delete(enum nb_event event,
						  const struct lyd_node *dnode)
{
	bool ipv4;
	struct isis_circuit *circuit;

	if (event != NB_EV_APPLY)
		return NB_OK;

	circuit = yang_dnode_get_entry(dnode, true);
	if (circuit->area) {
		ipv4 = yang_dnode_exists(dnode, "../ipv4-routing");
		isis_circuit_af_set(circuit, ipv4, false);
	}

	return NB_OK;
}

/*
 * XPath: /frr-interface:lib/interface/frr-isisd:isis/csnp-interval/level-1
 */
static int
lib_interface_isis_csnp_interval_level_1_modify(enum nb_event event,
						const struct lyd_node *dnode,
						union nb_resource *resource)
{
	/* TODO: implement me. */
	return NB_OK;
}

/*
 * XPath: /frr-interface:lib/interface/frr-isisd:isis/csnp-interval/level-2
 */
static int
lib_interface_isis_csnp_interval_level_2_modify(enum nb_event event,
						const struct lyd_node *dnode,
						union nb_resource *resource)
{
	/* TODO: implement me. */
	return NB_OK;
}

/*
 * XPath: /frr-interface:lib/interface/frr-isisd:isis/psnp-interval/level-1
 */
static int
lib_interface_isis_psnp_interval_level_1_modify(enum nb_event event,
						const struct lyd_node *dnode,
						union nb_resource *resource)
{
	/* TODO: implement me. */
	return NB_OK;
}

/*
 * XPath: /frr-interface:lib/interface/frr-isisd:isis/psnp-interval/level-2
 */
static int
lib_interface_isis_psnp_interval_level_2_modify(enum nb_event event,
						const struct lyd_node *dnode,
						union nb_resource *resource)
{
	/* TODO: implement me. */
	return NB_OK;
}

/*
 * XPath: /frr-interface:lib/interface/frr-isisd:isis/hello/padding
 */
static int lib_interface_isis_hello_padding_modify(enum nb_event event,
						   const struct lyd_node *dnode,
						   union nb_resource *resource)
{
	/* TODO: implement me. */
	return NB_OK;
}

/*
 * XPath: /frr-interface:lib/interface/frr-isisd:isis/hello/interval/level-1
 */
static int
lib_interface_isis_hello_interval_level_1_modify(enum nb_event event,
						 const struct lyd_node *dnode,
						 union nb_resource *resource)
{
	/* TODO: implement me. */
	return NB_OK;
}

/*
 * XPath: /frr-interface:lib/interface/frr-isisd:isis/hello/interval/level-2
 */
static int
lib_interface_isis_hello_interval_level_2_modify(enum nb_event event,
						 const struct lyd_node *dnode,
						 union nb_resource *resource)
{
	/* TODO: implement me. */
	return NB_OK;
}

/*
 * XPath: /frr-interface:lib/interface/frr-isisd:isis/hello/multiplier/level-1
 */
static int
lib_interface_isis_hello_multiplier_level_1_modify(enum nb_event event,
						   const struct lyd_node *dnode,
						   union nb_resource *resource)
{
	/* TODO: implement me. */
	return NB_OK;
}

/*
 * XPath: /frr-interface:lib/interface/frr-isisd:isis/hello/multiplier/level-2
 */
static int
lib_interface_isis_hello_multiplier_level_2_modify(enum nb_event event,
						   const struct lyd_node *dnode,
						   union nb_resource *resource)
{
	/* TODO: implement me. */
	return NB_OK;
}

/*
 * XPath: /frr-interface:lib/interface/frr-isisd:isis/metric/level-1
 */
static int
lib_interface_isis_metric_level_1_modify(enum nb_event event,
					 const struct lyd_node *dnode,
					 union nb_resource *resource)
{
	/* TODO: implement me. */
	return NB_OK;
}

/*
 * XPath: /frr-interface:lib/interface/frr-isisd:isis/metric/level-2
 */
static int
lib_interface_isis_metric_level_2_modify(enum nb_event event,
					 const struct lyd_node *dnode,
					 union nb_resource *resource)
{
	/* TODO: implement me. */
	return NB_OK;
}

/*
 * XPath: /frr-interface:lib/interface/frr-isisd:isis/priority/level-1
 */
static int
lib_interface_isis_priority_level_1_modify(enum nb_event event,
					   const struct lyd_node *dnode,
					   union nb_resource *resource)
{
	/* TODO: implement me. */
	return NB_OK;
}

/*
 * XPath: /frr-interface:lib/interface/frr-isisd:isis/priority/level-2
 */
static int
lib_interface_isis_priority_level_2_modify(enum nb_event event,
					   const struct lyd_node *dnode,
					   union nb_resource *resource)
{
	/* TODO: implement me. */
	return NB_OK;
}

/*
 * XPath: /frr-interface:lib/interface/frr-isisd:isis/network-type
 */
static int lib_interface_isis_network_type_modify(enum nb_event event,
						  const struct lyd_node *dnode,
						  union nb_resource *resource)
{
	/* TODO: implement me. */
	return NB_OK;
}

static int lib_interface_isis_network_type_delete(enum nb_event event,
						  const struct lyd_node *dnode)
{
	/* TODO: implement me. */
	return NB_OK;
}

/*
 * XPath: /frr-interface:lib/interface/frr-isisd:isis/passive
 */
static int lib_interface_isis_passive_create(enum nb_event event,
					     const struct lyd_node *dnode,
					     union nb_resource *resource)
{
	/* TODO: implement me. */
	return NB_OK;
}

static int lib_interface_isis_passive_delete(enum nb_event event,
					     const struct lyd_node *dnode)
{
	/* TODO: implement me. */
	return NB_OK;
}

/*
 * XPath: /frr-interface:lib/interface/frr-isisd:isis/password
 */
static int lib_interface_isis_password_create(enum nb_event event,
					      const struct lyd_node *dnode,
					      union nb_resource *resource)
{
	/* TODO: implement me. */
	return NB_OK;
}

static int lib_interface_isis_password_delete(enum nb_event event,
					      const struct lyd_node *dnode)
{
	/* TODO: implement me. */
	return NB_OK;
}

/*
 * XPath: /frr-interface:lib/interface/frr-isisd:isis/password/password
 */
static int
lib_interface_isis_password_password_modify(enum nb_event event,
					    const struct lyd_node *dnode,
					    union nb_resource *resource)
{
	/* TODO: implement me. */
	return NB_OK;
}

/*
 * XPath: /frr-interface:lib/interface/frr-isisd:isis/password/password-type
 */
static int
lib_interface_isis_password_password_type_modify(enum nb_event event,
						 const struct lyd_node *dnode,
						 union nb_resource *resource)
{
	/* TODO: implement me. */
	return NB_OK;
}

/*
 * XPath:
 * /frr-interface:lib/interface/frr-isisd:isis/disable-three-way-handshake
 */
static int lib_interface_isis_disable_three_way_handshake_create(
	enum nb_event event, const struct lyd_node *dnode,
	union nb_resource *resource)
{
	/* TODO: implement me. */
	return NB_OK;
}

static int lib_interface_isis_disable_three_way_handshake_delete(
	enum nb_event event, const struct lyd_node *dnode)
{
	/* TODO: implement me. */
	return NB_OK;
}

/*
 * XPath:
 * /frr-interface:lib/interface/frr-isisd:isis/multi-topology/ipv4-unicast
 */
static int lib_interface_isis_multi_topology_ipv4_unicast_modify(
	enum nb_event event, const struct lyd_node *dnode,
	union nb_resource *resource)
{
	/* TODO: implement me. */
	return NB_OK;
}

/*
 * XPath:
 * /frr-interface:lib/interface/frr-isisd:isis/multi-topology/ipv4-multicast
 */
static int lib_interface_isis_multi_topology_ipv4_multicast_modify(
	enum nb_event event, const struct lyd_node *dnode,
	union nb_resource *resource)
{
	/* TODO: implement me. */
	return NB_OK;
}

/*
 * XPath:
 * /frr-interface:lib/interface/frr-isisd:isis/multi-topology/ipv4-management
 */
static int lib_interface_isis_multi_topology_ipv4_management_modify(
	enum nb_event event, const struct lyd_node *dnode,
	union nb_resource *resource)
{
	/* TODO: implement me. */
	return NB_OK;
}

/*
 * XPath:
 * /frr-interface:lib/interface/frr-isisd:isis/multi-topology/ipv6-unicast
 */
static int lib_interface_isis_multi_topology_ipv6_unicast_modify(
	enum nb_event event, const struct lyd_node *dnode,
	union nb_resource *resource)
{
	/* TODO: implement me. */
	return NB_OK;
}

/*
 * XPath:
 * /frr-interface:lib/interface/frr-isisd:isis/multi-topology/ipv6-multicast
 */
static int lib_interface_isis_multi_topology_ipv6_multicast_modify(
	enum nb_event event, const struct lyd_node *dnode,
	union nb_resource *resource)
{
	/* TODO: implement me. */
	return NB_OK;
}

/*
 * XPath:
 * /frr-interface:lib/interface/frr-isisd:isis/multi-topology/ipv6-management
 */
static int lib_interface_isis_multi_topology_ipv6_management_modify(
	enum nb_event event, const struct lyd_node *dnode,
	union nb_resource *resource)
{
	/* TODO: implement me. */
	return NB_OK;
}

/*
 * XPath: /frr-interface:lib/interface/frr-isisd:isis/multi-topology/ipv6-dstsrc
 */
static int lib_interface_isis_multi_topology_ipv6_dstsrc_modify(
	enum nb_event event, const struct lyd_node *dnode,
	union nb_resource *resource)
{
	/* TODO: implement me. */
	return NB_OK;
}

/* clang-format off */
const struct frr_yang_module_info frr_isisd_info = {
	.name = "frr-isisd",
	.nodes = {
		{
			.xpath = "/frr-isisd:isis/instance",
			.cbs.create = isis_instance_create,
			.cbs.delete = isis_instance_delete,
			.cbs.cli_show = cli_show_router_isis,
			.priority = NB_DFLT_PRIORITY - 1,
		},
		{
			.xpath = "/frr-isisd:isis/instance/is-type",
			.cbs.modify = isis_instance_is_type_modify,
			.cbs.cli_show = cli_show_isis_is_type,
		},
		{
			.xpath = "/frr-isisd:isis/instance/area-address",
			.cbs.create = isis_instance_area_address_create,
			.cbs.delete = isis_instance_area_address_delete,
			.cbs.cli_show = cli_show_isis_area_address,
		},
		{
			.xpath = "/frr-isisd:isis/instance/dynamic-hostname",
			.cbs.modify = isis_instance_dynamic_hostname_modify,
			.cbs.cli_show = cli_show_isis_dynamic_hostname,
		},
		{
			.xpath = "/frr-isisd:isis/instance/attached",
			.cbs.create = isis_instance_attached_create,
			.cbs.delete = isis_instance_attached_delete,
			.cbs.cli_show = cli_show_isis_attached,
		},
		{
			.xpath = "/frr-isisd:isis/instance/overload",
			.cbs.create = isis_instance_overload_create,
			.cbs.delete = isis_instance_overload_delete,
			.cbs.cli_show = cli_show_isis_overload,
		},
		{
			.xpath = "/frr-isisd:isis/instance/metric-style",
			.cbs.modify = isis_instance_metric_style_modify,
			.cbs.cli_show = cli_show_isis_metric_style,
		},
		{
			.xpath = "/frr-isisd:isis/instance/purge-originator",
			.cbs.create = isis_instance_purge_originator_create,
			.cbs.delete = isis_instance_purge_originator_delete,
		},
		{
			.xpath = "/frr-isisd:isis/instance/lsp/mtu",
			.cbs.modify = isis_instance_lsp_mtu_modify,
		},
		{
			.xpath = "/frr-isisd:isis/instance/lsp/refresh-interval",
			.cbs.cli_show = cli_show_isis_lsp_ref_interval,
		},
		{
			.xpath = "/frr-isisd:isis/instance/lsp/refresh-interval/level-1",
			.cbs.modify = isis_instance_lsp_refresh_interval_level_1_modify,
		},
		{
			.xpath = "/frr-isisd:isis/instance/lsp/refresh-interval/level-2",
			.cbs.modify = isis_instance_lsp_refresh_interval_level_2_modify,
		},
		{
			.xpath = "/frr-isisd:isis/instance/lsp/maximum-lifetime/level-1",
			.cbs.modify = isis_instance_lsp_maximum_lifetime_level_1_modify,
		},
		{
			.xpath = "/frr-isisd:isis/instance/lsp/maximum-lifetime/level-2",
			.cbs.modify = isis_instance_lsp_maximum_lifetime_level_2_modify,
		},
		{
			.xpath = "/frr-isisd:isis/instance/lsp/generation-interval",
			.cbs.cli_show = cli_show_isis_lsp_gen_interval,
		},
		{
			.xpath = "/frr-isisd:isis/instance/lsp/generation-interval/level-1",
			.cbs.modify = isis_instance_lsp_generation_interval_level_1_modify,
		},
		{
			.xpath = "/frr-isisd:isis/instance/lsp/generation-interval/level-2",
			.cbs.modify = isis_instance_lsp_generation_interval_level_2_modify,
		},
		{
			.xpath = "/frr-isisd:isis/instance/spf/ietf-backoff-delay",
			.cbs.create = isis_instance_spf_ietf_backoff_delay_create,
			.cbs.delete = isis_instance_spf_ietf_backoff_delay_delete,
		},
		{
			.xpath = "/frr-isisd:isis/instance/spf/ietf-backoff-delay/init-delay",
			.cbs.modify = isis_instance_spf_ietf_backoff_delay_init_delay_modify,
		},
		{
			.xpath = "/frr-isisd:isis/instance/spf/ietf-backoff-delay/short-delay",
			.cbs.modify = isis_instance_spf_ietf_backoff_delay_short_delay_modify,
		},
		{
			.xpath = "/frr-isisd:isis/instance/spf/ietf-backoff-delay/long-delay",
			.cbs.modify = isis_instance_spf_ietf_backoff_delay_long_delay_modify,
		},
		{
			.xpath = "/frr-isisd:isis/instance/spf/ietf-backoff-delay/hold-down",
			.cbs.modify = isis_instance_spf_ietf_backoff_delay_hold_down_modify,
		},
		{
			.xpath = "/frr-isisd:isis/instance/spf/ietf-backoff-delay/time-to-learn",
			.cbs.modify = isis_instance_spf_ietf_backoff_delay_time_to_learn_modify,
		},
		{
			.xpath = "/frr-isisd:isis/instance/spf/minimum-interval/level-1",
			.cbs.modify = isis_instance_spf_minimum_interval_level_1_modify,
		},
		{
			.xpath = "/frr-isisd:isis/instance/spf/minimum-interval/level-2",
			.cbs.modify = isis_instance_spf_minimum_interval_level_2_modify,
		},
		{
			.xpath = "/frr-isisd:isis/instance/area-password",
			.cbs.create = isis_instance_area_password_create,
			.cbs.delete = isis_instance_area_password_delete,
			.cbs.apply_finish = area_password_apply_finish,
			.cbs.cli_show = cli_show_isis_area_pwd,
		},
		{
			.xpath = "/frr-isisd:isis/instance/area-password/password",
			.cbs.modify = isis_instance_area_password_password_modify,
		},
		{
			.xpath = "/frr-isisd:isis/instance/area-password/password-type",
			.cbs.modify = isis_instance_area_password_password_type_modify,
		},
		{
			.xpath = "/frr-isisd:isis/instance/area-password/authenticate-snp",
			.cbs.modify = isis_instance_area_password_authenticate_snp_modify,
		},
		{
			.xpath = "/frr-isisd:isis/instance/domain-password",
			.cbs.create = isis_instance_domain_password_create,
			.cbs.delete = isis_instance_domain_password_delete,
			.cbs.apply_finish = domain_password_apply_finish,
			.cbs.cli_show = cli_show_isis_domain_pwd,
		},
		{
			.xpath = "/frr-isisd:isis/instance/domain-password/password",
			.cbs.modify = isis_instance_domain_password_password_modify,
		},
		{
			.xpath = "/frr-isisd:isis/instance/domain-password/password-type",
			.cbs.modify = isis_instance_domain_password_password_type_modify,
		},
		{
			.xpath = "/frr-isisd:isis/instance/domain-password/authenticate-snp",
			.cbs.modify = isis_instance_domain_password_authenticate_snp_modify,
		},
		{
			.xpath = "/frr-isisd:isis/instance/default-information-originate/ipv4",
			.cbs.create = isis_instance_default_information_originate_ipv4_create,
			.cbs.delete = isis_instance_default_information_originate_ipv4_delete,
		},
		{
			.xpath = "/frr-isisd:isis/instance/default-information-originate/ipv4/always",
			.cbs.create = isis_instance_default_information_originate_ipv4_always_create,
			.cbs.delete = isis_instance_default_information_originate_ipv4_always_delete,
		},
		{
			.xpath = "/frr-isisd:isis/instance/default-information-originate/ipv4/route-map",
			.cbs.modify = isis_instance_default_information_originate_ipv4_route_map_modify,
			.cbs.delete = isis_instance_default_information_originate_ipv4_route_map_delete,
		},
		{
			.xpath = "/frr-isisd:isis/instance/default-information-originate/ipv4/metric",
			.cbs.modify = isis_instance_default_information_originate_ipv4_metric_modify,
			.cbs.delete = isis_instance_default_information_originate_ipv4_metric_delete,
		},
		{
			.xpath = "/frr-isisd:isis/instance/default-information-originate/ipv6",
			.cbs.create = isis_instance_default_information_originate_ipv6_create,
			.cbs.delete = isis_instance_default_information_originate_ipv6_delete,
		},
		{
			.xpath = "/frr-isisd:isis/instance/default-information-originate/ipv6/always",
			.cbs.create = isis_instance_default_information_originate_ipv6_always_create,
			.cbs.delete = isis_instance_default_information_originate_ipv6_always_delete,
		},
		{
			.xpath = "/frr-isisd:isis/instance/default-information-originate/ipv6/route-map",
			.cbs.modify = isis_instance_default_information_originate_ipv6_route_map_modify,
			.cbs.delete = isis_instance_default_information_originate_ipv6_route_map_delete,
		},
		{
			.xpath = "/frr-isisd:isis/instance/default-information-originate/ipv6/metric",
			.cbs.modify = isis_instance_default_information_originate_ipv6_metric_modify,
			.cbs.delete = isis_instance_default_information_originate_ipv6_metric_delete,
		},
		{
			.xpath = "/frr-isisd:isis/instance/redistribute/ipv4",
			.cbs.create = isis_instance_redistribute_ipv4_create,
			.cbs.delete = isis_instance_redistribute_ipv4_delete,
		},
		{
			.xpath = "/frr-isisd:isis/instance/redistribute/ipv4/route-map",
			.cbs.modify = isis_instance_redistribute_ipv4_route_map_modify,
			.cbs.delete = isis_instance_redistribute_ipv4_route_map_delete,
		},
		{
			.xpath = "/frr-isisd:isis/instance/redistribute/ipv4/metric",
			.cbs.modify = isis_instance_redistribute_ipv4_metric_modify,
			.cbs.delete = isis_instance_redistribute_ipv4_metric_delete,
		},
		{
			.xpath = "/frr-isisd:isis/instance/redistribute/ipv6",
			.cbs.create = isis_instance_redistribute_ipv6_create,
			.cbs.delete = isis_instance_redistribute_ipv6_delete,
		},
		{
			.xpath = "/frr-isisd:isis/instance/redistribute/ipv6/route-map",
			.cbs.modify = isis_instance_redistribute_ipv6_route_map_modify,
			.cbs.delete = isis_instance_redistribute_ipv6_route_map_delete,
		},
		{
			.xpath = "/frr-isisd:isis/instance/redistribute/ipv6/metric",
			.cbs.modify = isis_instance_redistribute_ipv6_metric_modify,
			.cbs.delete = isis_instance_redistribute_ipv6_metric_delete,
		},
		{
			.xpath = "/frr-isisd:isis/instance/multi-topology/ipv4-multicast",
			.cbs.create = isis_instance_multi_topology_ipv4_multicast_create,
			.cbs.delete = isis_instance_multi_topology_ipv4_multicast_delete,
		},
		{
			.xpath = "/frr-isisd:isis/instance/multi-topology/ipv4-multicast/overload",
			.cbs.create = isis_instance_multi_topology_ipv4_multicast_overload_create,
			.cbs.delete = isis_instance_multi_topology_ipv4_multicast_overload_delete,
		},
		{
			.xpath = "/frr-isisd:isis/instance/multi-topology/ipv4-management",
			.cbs.create = isis_instance_multi_topology_ipv4_management_create,
			.cbs.delete = isis_instance_multi_topology_ipv4_management_delete,
		},
		{
			.xpath = "/frr-isisd:isis/instance/multi-topology/ipv4-management/overload",
			.cbs.create = isis_instance_multi_topology_ipv4_management_overload_create,
			.cbs.delete = isis_instance_multi_topology_ipv4_management_overload_delete,
		},
		{
			.xpath = "/frr-isisd:isis/instance/multi-topology/ipv6-unicast",
			.cbs.create = isis_instance_multi_topology_ipv6_unicast_create,
			.cbs.delete = isis_instance_multi_topology_ipv6_unicast_delete,
		},
		{
			.xpath = "/frr-isisd:isis/instance/multi-topology/ipv6-unicast/overload",
			.cbs.create = isis_instance_multi_topology_ipv6_unicast_overload_create,
			.cbs.delete = isis_instance_multi_topology_ipv6_unicast_overload_delete,
		},
		{
			.xpath = "/frr-isisd:isis/instance/multi-topology/ipv6-multicast",
			.cbs.create = isis_instance_multi_topology_ipv6_multicast_create,
			.cbs.delete = isis_instance_multi_topology_ipv6_multicast_delete,
		},
		{
			.xpath = "/frr-isisd:isis/instance/multi-topology/ipv6-multicast/overload",
			.cbs.create = isis_instance_multi_topology_ipv6_multicast_overload_create,
			.cbs.delete = isis_instance_multi_topology_ipv6_multicast_overload_delete,
		},
		{
			.xpath = "/frr-isisd:isis/instance/multi-topology/ipv6-management",
			.cbs.create = isis_instance_multi_topology_ipv6_management_create,
			.cbs.delete = isis_instance_multi_topology_ipv6_management_delete,
		},
		{
			.xpath = "/frr-isisd:isis/instance/multi-topology/ipv6-management/overload",
			.cbs.create = isis_instance_multi_topology_ipv6_management_overload_create,
			.cbs.delete = isis_instance_multi_topology_ipv6_management_overload_delete,
		},
		{
			.xpath = "/frr-isisd:isis/instance/multi-topology/ipv6-dstsrc",
			.cbs.create = isis_instance_multi_topology_ipv6_dstsrc_create,
			.cbs.delete = isis_instance_multi_topology_ipv6_dstsrc_delete,
		},
		{
			.xpath = "/frr-isisd:isis/instance/multi-topology/ipv6-dstsrc/overload",
			.cbs.create = isis_instance_multi_topology_ipv6_dstsrc_overload_create,
			.cbs.delete = isis_instance_multi_topology_ipv6_dstsrc_overload_delete,
		},
		{
			.xpath = "/frr-isisd:isis/instance/log-adjacency-changes",
			.cbs.create = isis_instance_log_adjacency_changes_create,
			.cbs.delete = isis_instance_log_adjacency_changes_delete,
		},
		{
			.xpath = "/frr-isisd:isis/mpls-te",
			.cbs.create = isis_mpls_te_create,
			.cbs.delete = isis_mpls_te_delete,
		},
		{
			.xpath = "/frr-isisd:isis/mpls-te/router-address",
			.cbs.modify = isis_mpls_te_router_address_modify,
			.cbs.delete = isis_mpls_te_router_address_delete,
		},
		{
			.xpath = "/frr-interface:lib/interface/frr-isisd:isis",
			.cbs.create = lib_interface_isis_create,
			.cbs.delete = lib_interface_isis_delete,
		},
		{
			.xpath = "/frr-interface:lib/interface/frr-isisd:isis/area-tag",
			.cbs.modify = lib_interface_isis_area_tag_modify,
		},
		{
			.xpath = "/frr-interface:lib/interface/frr-isisd:isis/circuit-type",
			.cbs.modify = lib_interface_isis_circuit_type_modify,
		},
		{
			.xpath = "/frr-interface:lib/interface/frr-isisd:isis/ipv4-routing",
			.cbs.create = lib_interface_isis_ipv4_routing_create,
			.cbs.delete = lib_interface_isis_ipv4_routing_delete,
			.cbs.cli_show = cli_show_ip_isis_ipv4,
		},
		{
			.xpath = "/frr-interface:lib/interface/frr-isisd:isis/ipv6-routing",
			.cbs.create = lib_interface_isis_ipv6_routing_create,
			.cbs.delete = lib_interface_isis_ipv6_routing_delete,
			.cbs.cli_show = cli_show_ip_isis_ipv6,
		},
		{
			.xpath = "/frr-interface:lib/interface/frr-isisd:isis/csnp-interval/level-1",
			.cbs.modify = lib_interface_isis_csnp_interval_level_1_modify,
		},
		{
			.xpath = "/frr-interface:lib/interface/frr-isisd:isis/csnp-interval/level-2",
			.cbs.modify = lib_interface_isis_csnp_interval_level_2_modify,
		},
		{
			.xpath = "/frr-interface:lib/interface/frr-isisd:isis/psnp-interval/level-1",
			.cbs.modify = lib_interface_isis_psnp_interval_level_1_modify,
		},
		{
			.xpath = "/frr-interface:lib/interface/frr-isisd:isis/psnp-interval/level-2",
			.cbs.modify = lib_interface_isis_psnp_interval_level_2_modify,
		},
		{
			.xpath = "/frr-interface:lib/interface/frr-isisd:isis/hello/padding",
			.cbs.modify = lib_interface_isis_hello_padding_modify,
		},
		{
			.xpath = "/frr-interface:lib/interface/frr-isisd:isis/hello/interval/level-1",
			.cbs.modify = lib_interface_isis_hello_interval_level_1_modify,
		},
		{
			.xpath = "/frr-interface:lib/interface/frr-isisd:isis/hello/interval/level-2",
			.cbs.modify = lib_interface_isis_hello_interval_level_2_modify,
		},
		{
			.xpath = "/frr-interface:lib/interface/frr-isisd:isis/hello/multiplier/level-1",
			.cbs.modify = lib_interface_isis_hello_multiplier_level_1_modify,
		},
		{
			.xpath = "/frr-interface:lib/interface/frr-isisd:isis/hello/multiplier/level-2",
			.cbs.modify = lib_interface_isis_hello_multiplier_level_2_modify,
		},
		{
			.xpath = "/frr-interface:lib/interface/frr-isisd:isis/metric/level-1",
			.cbs.modify = lib_interface_isis_metric_level_1_modify,
		},
		{
			.xpath = "/frr-interface:lib/interface/frr-isisd:isis/metric/level-2",
			.cbs.modify = lib_interface_isis_metric_level_2_modify,
		},
		{
			.xpath = "/frr-interface:lib/interface/frr-isisd:isis/priority/level-1",
			.cbs.modify = lib_interface_isis_priority_level_1_modify,
		},
		{
			.xpath = "/frr-interface:lib/interface/frr-isisd:isis/priority/level-2",
			.cbs.modify = lib_interface_isis_priority_level_2_modify,
		},
		{
			.xpath = "/frr-interface:lib/interface/frr-isisd:isis/network-type",
			.cbs.modify = lib_interface_isis_network_type_modify,
			.cbs.delete = lib_interface_isis_network_type_delete,
		},
		{
			.xpath = "/frr-interface:lib/interface/frr-isisd:isis/passive",
			.cbs.create = lib_interface_isis_passive_create,
			.cbs.delete = lib_interface_isis_passive_delete,
		},
		{
			.xpath = "/frr-interface:lib/interface/frr-isisd:isis/password",
			.cbs.create = lib_interface_isis_password_create,
			.cbs.delete = lib_interface_isis_password_delete,
		},
		{
			.xpath = "/frr-interface:lib/interface/frr-isisd:isis/password/password",
			.cbs.modify = lib_interface_isis_password_password_modify,
		},
		{
			.xpath = "/frr-interface:lib/interface/frr-isisd:isis/password/password-type",
			.cbs.modify = lib_interface_isis_password_password_type_modify,
		},
		{
			.xpath = "/frr-interface:lib/interface/frr-isisd:isis/disable-three-way-handshake",
			.cbs.create = lib_interface_isis_disable_three_way_handshake_create,
			.cbs.delete = lib_interface_isis_disable_three_way_handshake_delete,
		},
		{
			.xpath = "/frr-interface:lib/interface/frr-isisd:isis/multi-topology/ipv4-unicast",
			.cbs.modify = lib_interface_isis_multi_topology_ipv4_unicast_modify,
		},
		{
			.xpath = "/frr-interface:lib/interface/frr-isisd:isis/multi-topology/ipv4-multicast",
			.cbs.modify = lib_interface_isis_multi_topology_ipv4_multicast_modify,
		},
		{
			.xpath = "/frr-interface:lib/interface/frr-isisd:isis/multi-topology/ipv4-management",
			.cbs.modify = lib_interface_isis_multi_topology_ipv4_management_modify,
		},
		{
			.xpath = "/frr-interface:lib/interface/frr-isisd:isis/multi-topology/ipv6-unicast",
			.cbs.modify = lib_interface_isis_multi_topology_ipv6_unicast_modify,
		},
		{
			.xpath = "/frr-interface:lib/interface/frr-isisd:isis/multi-topology/ipv6-multicast",
			.cbs.modify = lib_interface_isis_multi_topology_ipv6_multicast_modify,
		},
		{
			.xpath = "/frr-interface:lib/interface/frr-isisd:isis/multi-topology/ipv6-management",
			.cbs.modify = lib_interface_isis_multi_topology_ipv6_management_modify,
		},
		{
			.xpath = "/frr-interface:lib/interface/frr-isisd:isis/multi-topology/ipv6-dstsrc",
			.cbs.modify = lib_interface_isis_multi_topology_ipv6_dstsrc_modify,
		},
		{
			.xpath = NULL,
		},
	}
};
