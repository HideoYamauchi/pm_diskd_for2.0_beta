/*
 * Linux HA management library
 *
 * Author: Huang Zhen <zhenhltc@cn.ibm.com>
 * Copyright (c) 2005 International Business Machines
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <portability.h>

#include <unistd.h>
#include <glib.h>

#include <heartbeat.h>
#include <clplumbing/cl_log.h>
#include <clplumbing/cl_syslog.h>
#include <clplumbing/lsb_exitcodes.h>

#include "mgmt_internal.h"

#include <crm/cib.h>
#include <crm/msg_xml.h>
#include <crm/pengine/pengine.h>
#include <crm/pengine/pe_utils.h>

extern resource_t *group_find_child(resource_t *rsc, const char *id);

cib_t*	cib_conn = NULL;
int in_shutdown = FALSE;
int init_crm(int cache_cib);
void final_crm(void);

static void on_cib_diff(const char *event, HA_Message *msg);

static char* on_get_crm_config(char* argv[], int argc);
static char* on_update_crm_config(char* argv[], int argc);
static char* on_get_activenodes(char* argv[], int argc);
static char* on_get_dc(char* argv[], int argc);

static char* on_get_node_config(char* argv[], int argc);
static char* on_get_running_rsc(char* argv[], int argc);

static char* on_del_rsc(char* argv[], int argc);
static char* on_cleanup_rsc(char* argv[], int argc);
static char* on_add_rsc(char* argv[], int argc);
static char* on_add_grp(char* argv[], int argc);

static char* on_update_clone(char* argv[], int argc);
static char* on_get_clone(char* argv[], int argc);

static char* on_update_master(char* argv[], int argc);
static char* on_get_master(char* argv[], int argc);

static char* on_get_all_rsc(char* argv[], int argc);
static char* on_get_rsc_type(char* argv[], int argc);
static char* on_get_sub_rsc(char* argv[], int argc);
static char* on_get_rsc_attrs(char* argv[], int argc);
static char* on_get_rsc_running_on(char* argv[], int argc);
static char* on_get_rsc_status(char* argv[], int argc);

static char* on_get_rsc_params(char* argv[], int argc);
static char* on_update_rsc_params(char* argv[], int argc);
static char* on_delete_rsc_param(char* argv[], int argc);

static char* on_get_rsc_ops(char* argv[], int argc);
static char* on_update_rsc_ops(char* argv[], int argc);
static char* on_delete_rsc_op(char* argv[], int argc);

static char* on_get_constraints(char* argv[], int argc);
static char* on_get_constraint(char* argv[], int argc);
static char* on_update_constraint(char* argv[], int argc);
static char* on_delete_constraint(char* argv[], int argc);

static int delete_object(const char* type, const char* entry, const char* id, crm_data_t** output);
static GList* find_xml_node_list(crm_data_t *root, const char *search_path);
static int refresh_lrm(IPC_Channel *crmd_channel, const char *host_uname);
static int delete_lrm_rsc(IPC_Channel *crmd_channel, const char *host_uname, const char *rsc_id);
static pe_working_set_t* get_data_set(void);
static void free_data_set(pe_working_set_t* data_set);
static void on_cib_connection_destroy(gpointer user_data);
static char* failed_msg(crm_data_t* output, int rc);

pe_working_set_t* cib_cached = NULL;
int cib_cache_enable = FALSE;

#define GET_RESOURCE()	if (argc != 2) {				\
		return cl_strdup(MSG_FAIL);				\
	}								\
	rsc = pe_find_resource(data_set->resources, argv[1]);		\
	if (rsc == NULL) {						\
		return cl_strdup(MSG_FAIL);				\
	}

/* internal functions */
GList* find_xml_node_list(crm_data_t *root, const char *child_name)
{
	int i;
	GList* list = NULL;
	if (root == NULL) {
		return NULL;
	}
	for (i = 0; i < root->nfields; i++ ) {
		if (strncmp(root->names[i], child_name, MAX_STRLEN) == 0) {
			list = g_list_append(list, root->values[i]);
		}
	}
	return list;
}

int
delete_object(const char* type, const char* entry, const char* id, crm_data_t** output) 
{
	int rc;
	crm_data_t* cib_object = NULL;
	char xml[MAX_STRLEN];

	snprintf(xml, MAX_STRLEN, "<%s id=\"%s\">", entry, id);

	cib_object = string2xml(xml);
	if(cib_object == NULL) {
		return -1;
	}
	
	mgmt_log(LOG_DEBUG, "(delete)xml:%s",xml);

	rc = cib_conn->cmds->delete(
			cib_conn, type, cib_object, output, cib_sync_call);

	if (rc < 0) {
		return -1;
	}
	return 0;
}

pe_working_set_t*
get_data_set(void) 
{
	if (cib_cache_enable) {
		if (cib_cached != NULL) {
			return cib_cached;
		}
	}
	pe_working_set_t* data_set;
	
	data_set = (pe_working_set_t*)cl_malloc(sizeof(pe_working_set_t));
	set_working_set_defaults(data_set);
	data_set->input = get_cib_copy(cib_conn);
	data_set->now = new_ha_date(TRUE);
	stage0(data_set);
	
	if (cib_cache_enable) {
		cib_cached = data_set;
	}
	return data_set;
}

void 
free_data_set(pe_working_set_t* data_set)
{
	/* we only release the cib when cib is not cached.
	   the cached cib will be released in on_cib_diff() */
	if (!cib_cache_enable) {
		cleanup_calculations(data_set);
		cl_free(data_set);
	}
}	
char* 
failed_msg(crm_data_t* output, int rc) 
{
	const char* reason = NULL;
	crm_data_t* failed_tag;
	char* ret = cl_strdup(MSG_FAIL);
	
	ret = mgmt_msg_append(ret, cib_error2string(rc));
	
	if (output == NULL) {
		return ret;
	}
	
	failed_tag = cl_get_struct(output, XML_FAIL_TAG_CIB);
	if (failed_tag != NULL) {
		reason = ha_msg_value(failed_tag, XML_FAILCIB_ATTR_REASON);
		if (reason != NULL) {
			ret = mgmt_msg_append(ret, reason);
		}
	}
	free_xml(output);
	
	return ret;
}

/* mgmtd functions */
int
init_crm(int cache_cib)
{
	int ret = cib_ok;
	int i, max_try = 5;
	
	mgmt_log(LOG_INFO,"init_crm");
	crm_log_level = LOG_ERR;
	cib_conn = cib_new();
	in_shutdown = FALSE;
	
	cib_cache_enable = cache_cib?TRUE:FALSE;
	cib_cached = NULL;
	
	for (i = 0; i < max_try ; i++) {
		ret = cib_conn->cmds->signon(cib_conn, client_name, cib_command);
		if (ret == cib_ok) {
			break;
		}
		mgmt_log(LOG_INFO,"login to cib: %d, ret:%d",i,ret);
		sleep(1);
	}
	if (ret != cib_ok) {
		mgmt_log(LOG_INFO,"login to cib failed");
		cib_conn = NULL;
		return -1;
	}

	ret = cib_conn->cmds->add_notify_callback(cib_conn, T_CIB_DIFF_NOTIFY
						  , on_cib_diff);
	ret = cib_conn->cmds->set_connection_dnotify(cib_conn
			, on_cib_connection_destroy);

	reg_msg(MSG_CRM_CONFIG, on_get_crm_config);
	reg_msg(MSG_UP_CRM_CONFIG, on_update_crm_config);
	
	reg_msg(MSG_DC, on_get_dc);
	reg_msg(MSG_ACTIVENODES, on_get_activenodes);
	reg_msg(MSG_NODE_CONFIG, on_get_node_config);
	reg_msg(MSG_RUNNING_RSC, on_get_running_rsc);

	reg_msg(MSG_DEL_RSC, on_del_rsc);
	reg_msg(MSG_CLEANUP_RSC, on_cleanup_rsc);
	reg_msg(MSG_ADD_RSC, on_add_rsc);
	reg_msg(MSG_ADD_GRP, on_add_grp);
	
	reg_msg(MSG_ALL_RSC, on_get_all_rsc);
	reg_msg(MSG_SUB_RSC, on_get_sub_rsc);
	reg_msg(MSG_RSC_ATTRS, on_get_rsc_attrs);
	reg_msg(MSG_RSC_RUNNING_ON, on_get_rsc_running_on);
	reg_msg(MSG_RSC_STATUS, on_get_rsc_status);
	reg_msg(MSG_RSC_TYPE, on_get_rsc_type);
	
	reg_msg(MSG_RSC_PARAMS, on_get_rsc_params);
	reg_msg(MSG_UP_RSC_PARAMS, on_update_rsc_params);
	reg_msg(MSG_DEL_RSC_PARAM, on_delete_rsc_param);
	
	reg_msg(MSG_RSC_OPS, on_get_rsc_ops);
	reg_msg(MSG_UP_RSC_OPS, on_update_rsc_ops);
	reg_msg(MSG_DEL_RSC_OP, on_delete_rsc_op);

	reg_msg(MSG_UPDATE_CLONE, on_update_clone);
	reg_msg(MSG_GET_CLONE, on_get_clone);
	reg_msg(MSG_UPDATE_MASTER, on_update_master);
	reg_msg(MSG_GET_MASTER, on_get_master);

	reg_msg(MSG_GET_CONSTRAINTS, on_get_constraints);
	reg_msg(MSG_GET_CONSTRAINT, on_get_constraint);
	reg_msg(MSG_DEL_CONSTRAINT, on_delete_constraint);
	reg_msg(MSG_UP_CONSTRAINT, on_update_constraint);
	
	return 0;
}	
void
final_crm(void)
{
	if(cib_conn != NULL) {
		in_shutdown = TRUE;
		cib_conn->cmds->signoff(cib_conn);
		cib_conn = NULL;
	}
}

/* event handler */
void
on_cib_diff(const char *event, HA_Message *msg)
{
	if (debug_level) {
		mgmt_log(LOG_DEBUG,"update cib finished");
	}
	if (cib_cache_enable) {
		if (cib_cached != NULL) {
			cleanup_calculations(cib_cached);
			cl_free(cib_cached);
			cib_cached = NULL;
		}
	}
	
	fire_event(EVT_CIB_CHANGED);
}
void
on_cib_connection_destroy(gpointer user_data)
{
	fire_event(EVT_DISCONNECTED);
	cib_conn = NULL;
	if (!in_shutdown) {
		mgmt_log(LOG_ERR,"Connection to the CIB terminated... exiting");
		/*cib exits abnormally, mgmtd exits too and
		wait heartbeat	restart us in order*/
		exit(LSB_EXIT_OK);
	}
	return;
}

/* cluster  functions */
char* 
on_get_crm_config(char* argv[], int argc)
{
	char buf [255];
	pe_working_set_t* data_set;
	char* ret = cl_strdup(MSG_OK);
	data_set = get_data_set();
	
	ret = mgmt_msg_append(ret, data_set->transition_idle_timeout);
	ret = mgmt_msg_append(ret, data_set->symmetric_cluster?"True":"False");
	ret = mgmt_msg_append(ret, data_set->stonith_enabled?"True":"False");
	
	switch (data_set->no_quorum_policy) {
		case no_quorum_freeze:
			ret = mgmt_msg_append(ret, "freeze");
			break;
		case no_quorum_stop:
			ret = mgmt_msg_append(ret, "stop");
			break;
		case no_quorum_ignore:
			ret = mgmt_msg_append(ret, "ignore");
			break;
	}
	snprintf(buf, 255, "%d", data_set->default_resource_stickiness);
	ret = mgmt_msg_append(ret, buf);
	ret = mgmt_msg_append(ret, data_set->have_quorum?"True":"False");
	free_data_set(data_set);
	return ret;
}
char*
on_update_crm_config(char* argv[], int argc)
{
	int rc;
	crm_data_t* fragment = NULL;
	crm_data_t* cib_object = NULL;
	crm_data_t* output;
	char xml[MAX_STRLEN];

	ARGC_CHECK(3);
	snprintf(xml, MAX_STRLEN, "<nvpair id=\"%s\" name=\"%s\" value=\"%s\"/>", argv[1],argv[1],argv[2]);

	cib_object = string2xml(xml);
	if(cib_object == NULL) {
		return cl_strdup(MSG_FAIL);
	}

	fragment = create_cib_fragment(cib_object, "crm_config");

	mgmt_log(LOG_DEBUG, "(update)xml:%s",xml);

	rc = cib_conn->cmds->update(
			cib_conn, "crm_config", fragment, &output, cib_sync_call);

	if (rc < 0) {
		return failed_msg(output, rc);
	}

	return cl_strdup(MSG_OK);
}

/* node functions */
char*
on_get_activenodes(char* argv[], int argc)
{
	node_t* node;
	GList* cur;
	char* ret;
	pe_working_set_t* data_set;
	
	data_set = get_data_set();
	cur = data_set->nodes;
	ret = cl_strdup(MSG_OK);
	while (cur != NULL) {
		node = (node_t*) cur->data;
		if (node->details->online) {
			ret = mgmt_msg_append(ret, node->details->uname);
		}
		cur = g_list_next(cur);
	}
	free_data_set(data_set);
	return ret;
}

char* 
on_get_dc(char* argv[], int argc)
{
	pe_working_set_t* data_set;
	
	data_set = get_data_set();
	if (data_set->dc_node != NULL) {
		char* ret = cl_strdup(MSG_OK);
		ret = mgmt_msg_append(ret, data_set->dc_node->details->uname);
		free_data_set(data_set);
		return ret;
	}
	free_data_set(data_set);
	return cl_strdup(MSG_FAIL);
}


char*
on_get_node_config(char* argv[], int argc)
{
	node_t* node;
	GList* cur;
	pe_working_set_t* data_set;
	
	data_set = get_data_set();
	cur = data_set->nodes;
	ARGC_CHECK(2);
	while (cur != NULL) {
		node = (node_t*) cur->data;
		if (!node->details->online) {
			cur = g_list_next(cur);
			continue;
		}
		if (strncmp(argv[1],node->details->uname,MAX_STRLEN) == 0) {
			char* ret = cl_strdup(MSG_OK);
			ret = mgmt_msg_append(ret, node->details->uname);
			ret = mgmt_msg_append(ret, node->details->online?"True":"False");
			ret = mgmt_msg_append(ret, node->details->standby?"True":"False");
			ret = mgmt_msg_append(ret, node->details->unclean?"True":"False");
			ret = mgmt_msg_append(ret, node->details->shutdown?"True":"False");
			ret = mgmt_msg_append(ret, node->details->expected_up?"True":"False");
			ret = mgmt_msg_append(ret, node->details->is_dc?"True":"False");
			ret = mgmt_msg_append(ret, node->details->type==node_ping?"ping":"member");
			free_data_set(data_set);
			return ret;
		}
		cur = g_list_next(cur);
	}
	free_data_set(data_set);
	return cl_strdup(MSG_FAIL);
}

char*
on_get_running_rsc(char* argv[], int argc)
{
	node_t* node;
	GList* cur;
	pe_working_set_t* data_set;
	
	data_set = get_data_set();
	cur = data_set->nodes;
	ARGC_CHECK(2);
	while (cur != NULL) {
		node = (node_t*) cur->data;
		if (node->details->online) {
			if (strncmp(argv[1],node->details->uname,MAX_STRLEN) == 0) {
				GList* cur_rsc;
				char* ret = cl_strdup(MSG_OK);
				cur_rsc = node->details->running_rsc;
				while(cur_rsc != NULL) {
					resource_t* rsc = (resource_t*)cur_rsc->data;
					ret = mgmt_msg_append(ret, rsc->id);
					cur_rsc = g_list_next(cur_rsc);
				}
				free_data_set(data_set);
				return ret;
			}
		}
		cur = g_list_next(cur);
	}
	free_data_set(data_set);
	return cl_strdup(MSG_FAIL);
}

/* resource functions */
/* add/delete resource */
char*
on_del_rsc(char* argv[], int argc)
{
	int rc;
	resource_t* rsc;
	crm_data_t* cib_object = NULL;
	crm_data_t* output;
	char xml[MAX_STRLEN];
	pe_working_set_t* data_set;
	
	data_set = get_data_set();
	GET_RESOURCE()

	switch (rsc->variant) {
		case pe_native:
			snprintf(xml, MAX_STRLEN, "<primitive id=\"%s\"/>", rsc->id);
			break;
		case pe_group:
			snprintf(xml, MAX_STRLEN, "<group id=\"%s\"/>", rsc->id);
			break;
		case pe_clone:
			snprintf(xml, MAX_STRLEN, "<clone id=\"%s\"/>", rsc->id);
			break;
		case pe_master:
			snprintf(xml, MAX_STRLEN, "<master_slave id=\"%s\"/>", rsc->id);
			break;
		default:
			free_data_set(data_set);
			return cl_strdup(MSG_FAIL);
	}
	free_data_set(data_set);

	cib_object = string2xml(xml);
	if(cib_object == NULL) {
		return cl_strdup(MSG_FAIL);
	}

	mgmt_log(LOG_DEBUG, "(delete resources)xml:%s",xml);
	rc = cib_conn->cmds->delete(
			cib_conn, "resources", cib_object, &output, cib_sync_call);

	if (rc < 0) {
		return failed_msg(output, rc);
	}

	return cl_strdup(MSG_OK);
}
static int
delete_lrm_rsc(IPC_Channel *crmd_channel, const char *host_uname, const char *rsc_id)
{
	HA_Message *cmd = NULL;
	crm_data_t *msg_data = NULL;
	crm_data_t *rsc = NULL;
	char our_pid[11];
	char *key = NULL; 
	
	snprintf(our_pid, 10, "%d", getpid());
	our_pid[10] = '\0';
	key = crm_concat(client_name, our_pid, '-');
	
	
	msg_data = create_xml_node(NULL, XML_GRAPH_TAG_RSC_OP);
	crm_xml_add(msg_data, XML_ATTR_TRANSITION_KEY, key);
	
	rsc = create_xml_node(msg_data, XML_CIB_TAG_RESOURCE);
	crm_xml_add(rsc, XML_ATTR_ID, rsc_id);
	
	cmd = create_request(CRM_OP_LRM_DELETE, msg_data, host_uname,
			     CRM_SYSTEM_CRMD, client_name, our_pid);

	free_xml(msg_data);
	crm_free(key);

	if(send_ipc_message(crmd_channel, cmd)) {
		return 0;
	}
	return -1;
}

static int
refresh_lrm(IPC_Channel *crmd_channel, const char *host_uname)
{
	HA_Message *cmd = NULL;
	char our_pid[11];
	
	snprintf(our_pid, 10, "%d", getpid());
	our_pid[10] = '\0';
	
	cmd = create_request(CRM_OP_LRM_REFRESH, NULL, host_uname,
			     CRM_SYSTEM_CRMD, client_name, our_pid);
	
	if(send_ipc_message(crmd_channel, cmd)) {
		return 0;
	}
	return -1;
}

char*
on_cleanup_rsc(char* argv[], int argc)
{
	IPC_Channel *crmd_channel = NULL;
	char our_pid[11];
	
	snprintf(our_pid, 10, "%d", getpid());
	our_pid[10] = '\0';
	
	init_client_ipc_comms(CRM_SYSTEM_CRMD, NULL,
				    NULL, &crmd_channel);

	send_hello_message(crmd_channel, our_pid, client_name, "0", "1");
	delete_lrm_rsc(crmd_channel, argv[1], argv[2]);
	refresh_lrm(crmd_channel, argv[1]);
	return cl_strdup(MSG_OK);
}

/*
	0	cmd = "add_rsc"
	1	cmd += "\n"+rsc["id"]
	2	cmd += "\n"+rsc["class"]
	3	cmd += "\n"+rsc["type"]
	4	cmd += "\n"+rsc["provider"]
	5	cmd += "\n"+rsc["group"]
	6	cmd += "\n"+rsc["advance"]
	7	cmd += "\n"+rsc["advance_id"]
	8	cmd += "\n"+rsc["clone_max"]
	9	cmd += "\n"+rsc["clone_node_max"]
	10	cmd += "\n"+rsc["master_max"]
	11	cmd += "\n"+rsc["master_node_max"]
		for param in rsc["params"] :
	12,15,18...	cmd += "\n"+param["id"]
	13,16,19...	cmd += "\n"+param["name"]
	14,17,20...	cmd += "\n"+param["value"]
*/
char*
on_add_rsc(char* argv[], int argc)
{
	int rc, i, in_group;
	crm_data_t* fragment = NULL;
	crm_data_t* cib_object = NULL;
	crm_data_t* output = NULL;
	char xml[MAX_STRLEN];
	char buf[MAX_STRLEN];
	int clone, master, has_param;
		
	if (argc < 11) {
		return cl_strdup(MSG_FAIL);
	}
	xml[0]=0;
	in_group = (strlen(argv[5]) != 0);
	clone = (STRNCMP_CONST(argv[6], "clone") == 0);
	master = (STRNCMP_CONST(argv[6], "master") == 0);
	has_param = (argc > 11);
	if (in_group) {
		snprintf(buf, MAX_STRLEN, "<group id=\"%s\">", argv[5]);
		strncat(xml, buf, MAX_STRLEN);
	}
	if (clone) {
		snprintf(buf, MAX_STRLEN,
			 "<clone id=\"%s\"><instance_attributes><attributes>" \
			 "<nvpair id=\"clone_max\" name=\"clone_max\" value=\"%s\"/>" \
			 "<nvpair id=\"clone_node_max\" name=\"clone_node_max\" value=\"%s\"/>" \
			 "</attributes>	</instance_attributes> ",
			 argv[7], argv[8], argv[9]);
		strncat(xml, buf, MAX_STRLEN);
	}
	if (master) {
		snprintf(buf, MAX_STRLEN,
			 "<master_slave id=\"%s\"><instance_attributes><attributes>" \
			 "<nvpair id=\"clone_max\" name=\"clone_max\" value=\"%s\"/>" \
			 "<nvpair id=\"clone_node_max\" name=\"clone_node_max\" value=\"%s\"/>" \
			 "<nvpair id=\"master_max\" name=\"master_max\" value=\"%s\"/>" \
			 "<nvpair id=\"master_node_max\" name=\"master_node_max\" value=\"%s\"/>" \
			 "</attributes>	</instance_attributes>",
			 argv[7], argv[8], argv[9], argv[10], argv[11]);
		strncat(xml, buf, MAX_STRLEN);
	}
	
	if (!has_param) {
		snprintf(buf, MAX_STRLEN,
			 "<primitive id=\"%s\" class=\"%s\" type=\"%s\" provider=\"%s\"/>"
					 , argv[1],argv[2], argv[3],argv[4]);
		strncat(xml, buf, MAX_STRLEN);
	}
	else {
		snprintf(buf, MAX_STRLEN,
			 "<primitive id=\"%s\" class=\"%s\" type=\"%s\" provider=\"%s\">" \
			 "<instance_attributes> <attributes>"
			 , argv[1],argv[2], argv[3],argv[4]);
		strncat(xml, buf, MAX_STRLEN);
	
		for (i = 12; i < argc; i += 3) {
			snprintf(buf, MAX_STRLEN,
				 "<nvpair id=\"%s\" name=\"%s\" value=\"%s\"/>",
				 argv[i], argv[i+1],argv[i+2]);
			strncat(xml, buf, MAX_STRLEN);
		}
		strncat(xml, "</attributes></instance_attributes></primitive>", MAX_STRLEN);
	}
	if (master) {
		strncat(xml, "</master_slave>", MAX_STRLEN);
	}
	if (clone) {
		strncat(xml, "</clone>", MAX_STRLEN);
	}
	
	if (in_group) {
		strncat(xml, "</group>", MAX_STRLEN);
	}
	
	cib_object = string2xml(xml);
	if(cib_object == NULL) {
		return cl_strdup(MSG_FAIL);
	}
	mgmt_log(LOG_INFO, "xml:%s",xml);
	fragment = create_cib_fragment(cib_object, "resources");

	if (in_group) {
		rc = cib_conn->cmds->update(
			cib_conn, "resources", fragment, &output, cib_sync_call);
	}
	else {
		rc = cib_conn->cmds->create(
			cib_conn, "resources", fragment, &output, cib_sync_call);
	}
	
	if (rc < 0) {
		return failed_msg(output, rc);
	}
	return cl_strdup(MSG_OK);

}
char*
on_add_grp(char* argv[], int argc)
{
	int rc;
	crm_data_t* fragment = NULL;
	crm_data_t* cib_object = NULL;
	crm_data_t* output;
	char xml[MAX_STRLEN];
	
	ARGC_CHECK(2);
	snprintf(xml, MAX_STRLEN,"<group id=\"%s\"/>", argv[1]);
	cib_object = string2xml(xml);
	if(cib_object == NULL) {
		return cl_strdup(MSG_FAIL);
	}
	mgmt_log(LOG_INFO, "xml:%s",xml);
	fragment = create_cib_fragment(cib_object, "resources");
	rc = cib_conn->cmds->create(cib_conn, "resources", fragment, &output, cib_sync_call);
	if (rc < 0) {
		return failed_msg(output, rc);
	}
	return cl_strdup(MSG_OK);

}
/* get all resources*/
char*
on_get_all_rsc(char* argv[], int argc)
{
	GList* cur;
	char* ret;
	pe_working_set_t* data_set;
	
	data_set = get_data_set();
	ret = cl_strdup(MSG_OK);
	cur = data_set->resources;
	while (cur != NULL) {
		resource_t* rsc = (resource_t*)cur->data;
		if(rsc->orphan == FALSE || rsc->role != RSC_ROLE_STOPPED) {
			ret = mgmt_msg_append(ret, rsc->id);
		}
		cur = g_list_next(cur);
	}
	free_data_set(data_set);
	return ret;
}
/* basic information of resource */
char*
on_get_rsc_attrs(char* argv[], int argc)
{
	resource_t* rsc;
	char* ret;
	struct ha_msg* attrs;
	pe_working_set_t* data_set;
	
	data_set = get_data_set();
	GET_RESOURCE()

	ret = cl_strdup(MSG_OK);
	attrs = (struct ha_msg*)rsc->xml;
	ret = mgmt_msg_append(ret, ha_msg_value(attrs, "id"));
	ret = mgmt_msg_append(ret, ha_msg_value(attrs, "class"));
	ret = mgmt_msg_append(ret, ha_msg_value(attrs, "provider"));
	ret = mgmt_msg_append(ret, ha_msg_value(attrs, "type"));
	free_data_set(data_set);
	return ret;
}

char*
on_get_rsc_running_on(char* argv[], int argc)
{
	resource_t* rsc;
	char* ret;
	GList* cur;
	pe_working_set_t* data_set;
	
	data_set = get_data_set();
	GET_RESOURCE()

	ret = cl_strdup(MSG_OK);
	cur = rsc->running_on;
	while (cur != NULL) {
		node_t* node = (node_t*)cur->data;
		ret = mgmt_msg_append(ret, node->details->uname);
		cur = g_list_next(cur);
	}
	free_data_set(data_set);
	return ret;
}
char*
on_get_rsc_status(char* argv[], int argc)
{
	resource_t* rsc;
	char* ret;
	pe_working_set_t* data_set;
	
	data_set = get_data_set();
	GET_RESOURCE()

	ret = cl_strdup(MSG_OK);
	switch (rsc->variant) {
		case pe_unknown:
			ret = mgmt_msg_append(ret, "unknown");
			break;
		case pe_native:
			if(rsc->is_managed == FALSE) {
				ret = mgmt_msg_append(ret, "unmanaged");
				break;
			}
			if( rsc->failed ) {
				ret = mgmt_msg_append(ret, "failed");
				break;
			}
			if( g_list_length(rsc->running_on) == 0) {
				ret = mgmt_msg_append(ret, "not running");
				break;
			}
			if( g_list_length(rsc->running_on) > 1) {
				ret = mgmt_msg_append(ret, "multi-running");
				break;
			}
			ret = mgmt_msg_append(ret, "running");		
			break;
		case pe_group:
			ret = mgmt_msg_append(ret, "group");
			break;
		case pe_clone:
			ret = mgmt_msg_append(ret, "clone");
			break;
		case pe_master:
			ret = mgmt_msg_append(ret, "master");
			break;
	}
	free_data_set(data_set);
	return ret;
}

char*
on_get_rsc_type(char* argv[], int argc)
{
	resource_t* rsc;
	char* ret;
	pe_working_set_t* data_set;
	
	data_set = get_data_set();
	GET_RESOURCE()

	ret = cl_strdup(MSG_OK);

	switch (rsc->variant) {
		case pe_unknown:
			ret = mgmt_msg_append(ret, "unknown");
			break;
		case pe_native:
			ret = mgmt_msg_append(ret, "native");
			break;
		case pe_group:
			ret = mgmt_msg_append(ret, "group");
			break;
		case pe_clone:
			ret = mgmt_msg_append(ret, "clone");
			break;
		case pe_master:
			ret = mgmt_msg_append(ret, "master");
			break;
	}
	free_data_set(data_set);
	return ret;
}

char*
on_get_sub_rsc(char* argv[], int argc)
{
	resource_t* rsc;
	char* ret;
	GList* cur = NULL;
	pe_working_set_t* data_set;
	
	data_set = get_data_set();
	GET_RESOURCE()
		
	cur = rsc->fns->children(rsc);
	
	ret = cl_strdup(MSG_OK);
	while (cur != NULL) {
		resource_t* rsc = (resource_t*)cur->data;
		ret = mgmt_msg_append(ret, rsc->id);
		cur = g_list_next(cur);
	}
	free_data_set(data_set);
	return ret;
}

/* resource params */
char*
on_get_rsc_params(char* argv[], int argc)
{
	int i;
	resource_t* rsc;
	char* ret;
	struct ha_msg* attrs;
	struct ha_msg* nvpair;
	pe_working_set_t* data_set;
	
	data_set = get_data_set();
	GET_RESOURCE()

	ret = cl_strdup(MSG_OK);
	attrs = cl_get_struct((struct ha_msg*)rsc->xml, "instance_attributes");
	if(attrs == NULL) {
		free_data_set(data_set);
		return ret;
	}
	attrs = cl_get_struct(attrs, "attributes");
	if(attrs == NULL) {
		free_data_set(data_set);
		return ret;
	}
	for (i = 0; i < attrs->nfields; i++) {
		if (STRNCMP_CONST(attrs->names[i], "nvpair") == 0) {
			nvpair = (struct ha_msg*)attrs->values[i];
			ret = mgmt_msg_append(ret, ha_msg_value(nvpair, "id"));
			ret = mgmt_msg_append(ret, ha_msg_value(nvpair, "name"));
			ret = mgmt_msg_append(ret, ha_msg_value(nvpair, "value"));
		}
	}
	free_data_set(data_set);
	return ret;
}
char*
on_update_rsc_params(char* argv[], int argc)
{
	int rc, i;
	crm_data_t* fragment = NULL;
	crm_data_t* cib_object = NULL;
	crm_data_t* output;
	char xml[MAX_STRLEN];
	char buf[MAX_STRLEN];

	snprintf(xml, MAX_STRLEN,
 		 "<primitive id=\"%s\">"
    		 "<instance_attributes><attributes>", argv[1]);
	for (i = 2; i < argc; i += 3) {
		snprintf(buf, MAX_STRLEN,
			"<nvpair id=\"%s\" name=\"%s\" value=\"%s\"/>",
			argv[i], argv[i+1], argv[i+2]);
		strncat(xml, buf, MAX_STRLEN);
	}
	strncat(xml, "</attributes></instance_attributes></primitive>", MAX_STRLEN);

	cib_object = string2xml(xml);
	if(cib_object == NULL) {
		return cl_strdup(MSG_FAIL);
	}
	mgmt_log(LOG_INFO, "xml:%s",xml);
	fragment = create_cib_fragment(cib_object, "resources");

	rc = cib_conn->cmds->update(
			cib_conn, "resources", fragment, &output, cib_sync_call);

	if (rc < 0) {
		return failed_msg(output, rc);
	}
	return cl_strdup(MSG_OK);
}
char*
on_delete_rsc_param(char* argv[], int argc)
{
	crm_data_t * output;
	int rc;
	ARGC_CHECK(2)

	if ((rc=delete_object("resources", "nvpair", argv[1], &output)) < 0) {
		return failed_msg(output, rc);
	}
	return cl_strdup(MSG_OK);
}
/* resource operations */
char*
on_get_rsc_ops(char* argv[], int argc)
{
	int i;
	resource_t* rsc;
	char* ret;
	struct ha_msg* ops;
	struct ha_msg* op;
	pe_working_set_t* data_set;
	
	data_set = get_data_set();
	GET_RESOURCE()

	ret = cl_strdup(MSG_OK);
	ops = cl_get_struct((struct ha_msg*)rsc->xml, "operations");
	if (ops == NULL) {
		free_data_set(data_set);
		return ret;
	}
	for (i = 0; i < ops->nfields; i++) {
		if (STRNCMP_CONST(ops->names[i], "op") == 0) {
			op = (struct ha_msg*)ops->values[i];
			ret = mgmt_msg_append(ret, ha_msg_value(op, "id"));
			ret = mgmt_msg_append(ret, ha_msg_value(op, "name"));
			ret = mgmt_msg_append(ret, ha_msg_value(op, "interval"));
			ret = mgmt_msg_append(ret, ha_msg_value(op, "timeout"));
		}
	}
	free_data_set(data_set);
	return ret;
}
char*
on_update_rsc_ops(char* argv[], int argc)
{
	int rc, i;
	crm_data_t* fragment = NULL;
	crm_data_t* cib_object = NULL;
	crm_data_t* output;
	char xml[MAX_STRLEN];
	char buf[MAX_STRLEN];

	snprintf(xml, MAX_STRLEN,
 		 "<primitive id=\"%s\">"
    		 " <operations>", argv[1]);
	for (i = 2; i < argc; i += 4) {
		snprintf(buf, MAX_STRLEN,
			"<op id=\"%s\" name=\"%s\" interval=\"%s\" timeout=\"%s\"/>",
			argv[i], argv[i+1], argv[i+2], argv[i+3]);
		strncat(xml, buf, MAX_STRLEN);
	}
	strncat(xml, "</operations></primitive>", MAX_STRLEN);

	cib_object = string2xml(xml);
	if(cib_object == NULL) {
		return cl_strdup(MSG_FAIL);
	}
	mgmt_log(LOG_INFO, "xml:%s",xml);
	fragment = create_cib_fragment(cib_object, "resources");

	rc = cib_conn->cmds->update(
			cib_conn, "resources", fragment, &output, cib_sync_call);

	if (rc < 0) {
		return failed_msg(output, rc);
	}
	return cl_strdup(MSG_OK);
}
char*
on_delete_rsc_op(char* argv[], int argc)
{
	int rc;
	crm_data_t * output;
	ARGC_CHECK(2)

	if ((rc=delete_object("resources", "op", argv[1], &output)) < 0) {
		return failed_msg(output, rc);
	}
	return cl_strdup(MSG_OK);
}
/* clone functions */
char*
on_get_clone(char* argv[], int argc)
{
	resource_t* rsc;
	char* ret;
	char* parameter=NULL;
	pe_working_set_t* data_set;
	
	data_set = get_data_set();
	GET_RESOURCE()

	ret = cl_strdup(MSG_OK);
	ret = mgmt_msg_append(ret, rsc->id);

	parameter = rsc->fns->parameter(rsc, NULL, FALSE
	,	XML_RSC_ATTR_INCARNATION_MAX, data_set);
	ret = mgmt_msg_append(ret, parameter);
	cl_free(parameter);
	
	parameter = rsc->fns->parameter(rsc, NULL, FALSE
	,	XML_RSC_ATTR_INCARNATION_NODEMAX, data_set);
	ret = mgmt_msg_append(ret, parameter);
	cl_free(parameter);

	free_data_set(data_set);
	return ret;
}
char*
on_update_clone(char* argv[], int argc)
{
	int rc;
	crm_data_t* fragment = NULL;
	crm_data_t* cib_object = NULL;
	crm_data_t* output;
	char xml[MAX_STRLEN];

	ARGC_CHECK(4);
	snprintf(xml,MAX_STRLEN,
		 "<clone id=\"%s\"><instance_attributes><attributes>" \
		 "<nvpair id=\"clone_max\" name=\"clone_max\" value=\"%s\"/>" \
		 "<nvpair id=\"clone_node_max\" name=\"clone_node_max\" value=\"%s\"/>" \
		 "</attributes></instance_attributes></clone>",
		 argv[1],argv[2],argv[3]);

	cib_object = string2xml(xml);
	if(cib_object == NULL) {
		return cl_strdup(MSG_FAIL);
	}
	mgmt_log(LOG_INFO, "xml:%s",xml);
	fragment = create_cib_fragment(cib_object, "resources");
	rc = cib_conn->cmds->update(cib_conn, "resources", fragment, &output, cib_sync_call);
	if (rc < 0) {
		return failed_msg(output, rc);
	}
	return cl_strdup(MSG_OK);
}
/* master functions */
char*
on_get_master(char* argv[], int argc)
{
	resource_t* rsc;
	char* ret;
	char* parameter=NULL;
	pe_working_set_t* data_set;
	
	data_set = get_data_set();
	GET_RESOURCE()
	
	ret = cl_strdup(MSG_OK);
	ret = mgmt_msg_append(ret, rsc->id);
	
	parameter = rsc->fns->parameter(rsc, NULL, FALSE
	,	XML_RSC_ATTR_INCARNATION_MAX, data_set);
	ret = mgmt_msg_append(ret, parameter);
	cl_free(parameter);

	parameter = rsc->fns->parameter(rsc, NULL, FALSE
	,	XML_RSC_ATTR_INCARNATION_NODEMAX, data_set);
	ret = mgmt_msg_append(ret, parameter);
	cl_free(parameter);

	parameter = rsc->fns->parameter(rsc, NULL, FALSE
	,	XML_RSC_ATTR_MASTER_MAX, data_set);
	ret = mgmt_msg_append(ret, parameter);
	cl_free(parameter);

	parameter = rsc->fns->parameter(rsc, NULL, FALSE
	,	XML_RSC_ATTR_MASTER_NODEMAX, data_set);
	ret = mgmt_msg_append(ret, parameter);
	cl_free(parameter);

	free_data_set(data_set);
	return ret;
}
char*
on_update_master(char* argv[], int argc)
{
	int rc;
	crm_data_t* fragment = NULL;
	crm_data_t* cib_object = NULL;
	crm_data_t* output;
	char xml[MAX_STRLEN];

	ARGC_CHECK(6);
	snprintf(xml,MAX_STRLEN,
		 "<master_slave id=\"%s\"><instance_attributes><attributes>" \
		 "<nvpair id=\"clone_max\" name=\"clone_max\" value=\"%s\"/>" \
		 "<nvpair id=\"clone_node_max\" name=\"clone_node_max\" value=\"%s\"/>" \
		 "<nvpair id=\"master_max\" name=\"master_max\" value=\"%s\"/>" \
		 "<nvpair id=\"master_node_max\" name=\"master_node_max\" value=\"%s\"/>" \
		 "</attributes></instance_attributes></master_slave>",
		 argv[1],argv[2],argv[3],argv[4],argv[5]);

	cib_object = string2xml(xml);
	if(cib_object == NULL) {
		return cl_strdup(MSG_FAIL);
	}
	mgmt_log(LOG_INFO, "xml:%s",xml);
	fragment = create_cib_fragment(cib_object, "resources");
	rc = cib_conn->cmds->update(cib_conn, "resources", fragment, &output, cib_sync_call);
	if (rc < 0) {
		return failed_msg(output, rc);
	}
	return cl_strdup(MSG_OK);

}

/* constraints functions */
char*
on_get_constraints(char* argv[], int argc)
{
	char* ret;
	GList* list;
	GList* cur;
	crm_data_t* cos = NULL;
	pe_working_set_t* data_set;
	const char* path[] = {"configuration","constraints"}
	
	ARGC_CHECK(2);
	
	data_set = get_data_set();
	cos = find_xml_node_nested(data_set->input, path, 2);
	if (cos == NULL) {
		free_data_set(data_set);
		return  cl_strdup(MSG_FAIL);
	}
	ret = cl_strdup(MSG_OK);
	list = find_xml_node_list(cos, argv[1]);
	cur = list;
	while (cur != NULL) {
		crm_data_t* location = (crm_data_t*)cur->data;
		ret = mgmt_msg_append(ret, ha_msg_value(location, "id"));
		
		cur = g_list_next(cur);
	}
	g_list_free(list);
	free_data_set(data_set);
	return ret;
}

char*
on_get_constraint(char* argv[], int argc)
{
	char* ret;
	GList* list;
	GList* cur;
	crm_data_t* rule;
	
	GList* expr_list, *expr_cur;
	crm_data_t* cos = NULL;
	pe_working_set_t* data_set;
	const char* path[] = {"configuration","constraints"}
	ARGC_CHECK(3);
	
	data_set = get_data_set();
	cos = find_xml_node_nested(data_set->input, path, 2);
	if (cos == NULL) {
		free_data_set(data_set);
		return  cl_strdup(MSG_FAIL);
	}
	ret = cl_strdup(MSG_OK);
	list = find_xml_node_list(cos, argv[1]);
	cur = list;
	while (cur != NULL) {
		crm_data_t* constraint = (crm_data_t*)cur->data;
		if (strncmp(argv[2],ha_msg_value(constraint, "id"), MAX_STRLEN)==0) {
			if (STRNCMP_CONST(argv[1],"rsc_location")==0) {
				ret = mgmt_msg_append(ret, ha_msg_value(constraint, "id"));
				ret = mgmt_msg_append(ret, ha_msg_value(constraint, "rsc"));
				rule = find_xml_node(constraint,"rule",TRUE);
				ret = mgmt_msg_append(ret, ha_msg_value(rule, "score"));
				expr_list = find_xml_node_list(rule, "expression");
				expr_cur = expr_list;
				while(expr_cur) {
					crm_data_t* expr = (crm_data_t*)expr_cur->data;
					ret = mgmt_msg_append(ret, ha_msg_value(expr, "id"));
					ret = mgmt_msg_append(ret, ha_msg_value(expr, "attribute"));
					ret = mgmt_msg_append(ret, ha_msg_value(expr, "operation"));
					ret = mgmt_msg_append(ret, ha_msg_value(expr, "value"));
					expr_cur = g_list_next(expr_cur);
				}
				g_list_free(expr_list);
			}
			else if (STRNCMP_CONST(argv[1],"rsc_order")==0) {
				ret = mgmt_msg_append(ret, ha_msg_value(constraint, "id"));
				ret = mgmt_msg_append(ret, ha_msg_value(constraint, "from"));
				ret = mgmt_msg_append(ret, ha_msg_value(constraint, "type"));
				ret = mgmt_msg_append(ret, ha_msg_value(constraint, "to"));
			}
			else if (STRNCMP_CONST(argv[1],"rsc_colocation")==0) {
				ret = mgmt_msg_append(ret, ha_msg_value(constraint, "id"));
				ret = mgmt_msg_append(ret, ha_msg_value(constraint, "from"));
				ret = mgmt_msg_append(ret, ha_msg_value(constraint, "to"));
				ret = mgmt_msg_append(ret, ha_msg_value(constraint, "score"));
			}
			break;
		}
		cur = g_list_next(cur);
	}
	g_list_free(list);
	free_data_set(data_set);
	return ret;
}
char*
on_delete_constraint(char* argv[], int argc)
{
	int rc;
	crm_data_t * output;
	ARGC_CHECK(3)

	if ((rc=delete_object("constraints", argv[1], argv[2], &output)) < 0) {
		return failed_msg(output, rc);
	}
	return cl_strdup(MSG_OK);
}

char*
on_update_constraint(char* argv[], int argc)
{
	int rc;
	crm_data_t* fragment = NULL;
	crm_data_t* cib_object = NULL;
	crm_data_t* output;
	int i;
	char xml[MAX_STRLEN];

	if (STRNCMP_CONST(argv[1],"rsc_location")==0) {
		snprintf(xml, MAX_STRLEN,
			 "<rsc_location id=\"%s\" rsc=\"%s\">" \
				"<rule id=\"prefered_%s\" score=\"%s\">",
		 	 argv[2], argv[3], argv[2], argv[4]);
		for (i = 0; i < (argc-5)/4; i++) {
			char expr[MAX_STRLEN];
			snprintf(expr, MAX_STRLEN,
				 "<expression attribute=\"%s\" id=\"%s\" operation=\"%s\" value=\"%s\"/>",
			 	 argv[5+i*4+1],argv[5+i*4],argv[5+i*4+2],argv[5+i*4+3]);
			strncat(xml, expr, MAX_STRLEN);
		}
		strncat(xml, "</rule></rsc_location>", MAX_STRLEN);
	}
	else if (STRNCMP_CONST(argv[1],"rsc_order")==0) {
		snprintf(xml, MAX_STRLEN,
			 "<rsc_order id=\"%s\" from=\"%s\" type=\"%s\" to=\"%s\"/>",
			 argv[2], argv[3], argv[4], argv[5]);
	}
	else if (STRNCMP_CONST(argv[1],"rsc_colocation")==0) {
		snprintf(xml, MAX_STRLEN,
			 "<rsc_colocation id=\"%s\" from=\"%s\" to=\"%s\" score=\"%s\"/>",
			 argv[2], argv[3], argv[4], argv[5]);
	}
	cib_object = string2xml(xml);
	if(cib_object == NULL) {
		return cl_strdup(MSG_FAIL);
	}
	mgmt_log(LOG_INFO, "xml:%s",xml);
	fragment = create_cib_fragment(cib_object, "constraints");

	rc = cib_conn->cmds->update(
			cib_conn, "constraints", fragment, &output, cib_sync_call);

	if (rc < 0) {
		return failed_msg(output, rc);
	}
	return cl_strdup(MSG_OK);
}

