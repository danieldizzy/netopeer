/**
 * \file server_operations.h
 * \author Radek Krejci <rkrejci@cesent.cz>
 * \brief Netopeer server operations definitions.
 *
 * Copyright (C) 2011 CESNET, z.s.p.o.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name of the Company nor the names of its contributors
 *    may be used to endorse or promote products derived from this
 *    software without specific prior written permission.
 *
 * ALTERNATIVELY, provided that this notice is retained in full, this
 * product may be distributed under the terms of the GNU General Public
 * License (GPL) version 2 or later, in which case the provisions
 * of the GPL apply INSTEAD OF those given above.
 *
 * This software is provided ``as is, and any express or implied
 * warranties, including, but not limited to, the implied warranties of
 * merchantability and fitness for a particular purpose are disclaimed.
 * In no event shall the company or contributors be liable for any
 * direct, indirect, incidental, special, exemplary, or consequential
 * damages (including, but not limited to, procurement of substitute
 * goods or services; loss of use, data, or profits; or business
 * interruption) however caused and on any theory of liability, whether
 * in contract, strict liability, or tort (including negligence or
 * otherwise) arising in any way out of the use of this software, even
 * if advised of the possibility of such damage.
 *
 */

#ifndef SERVER_OP_H_
#define SERVER_OP_H_

#include <libnetconf_xml.h>
#include <libnetconf/datastore.h>
#include <dbus/dbus.h>
#include <libxml/tree.h>

/* VERBOSE macro */
char err_msg[4096];
#define VERB(level,format,args...) if(verbose>=level){snprintf(err_msg,4095,format,##args); clb_print(level,err_msg);}

struct session_info {
	/**
	 * String identifying netopeer agent on D-BUS
	 */
	char *dbus_id;
	/**
	 * Pointer to library provided session.
	 * In our architecture are sessions are dummy
	 * and can not be used for communication.
	 * We use dbus instead.
	 */
	struct nc_session * session;
	/**
	 * Doubly linked list links.
	 */
	struct session_info *next, *prev;
};

struct server_module {
	/**
	 * Name of the device configuration module according to Netopeer's internal
	 * configuration.
	 */
	char *name;
	/**
	 * List of capabilities associated with the device module. All module specific
	 * operations or data associated with one of the capabilities will be passed into
	 * the module.
	 */
	struct nc_cpblts *cpblts;
	/**
	 * Unique random ID for each configuration plugin generated by Netopeer
	 * configuration module.
	 * 0 for modules providing only state information (w/o <repo> tag in configuration).
	 */
	ncds_id repo_id;
	/**
	 * Device module ID, used to allow device module applying rpcs.
	 */
	int device_module_id;
	/**
	 * Is the module allowed. If set to zero module will not be delivered with any rpc message.
	 * Configurable over Netopeer device module.
	 * It is recommended to not forbid Netopeer module although it is possible ;)
	 */
	int allowed;
	/**
	 * Null terminated list of rpc operation implemented by device module
	 */
	char ** implemented_rpcs;
	/**
	 * Is module functionality implemented as a transapi module?
	 * Every module must be implemented either as transapi module or
	 * as old style server module.
	 */
	int transapi;
	/**
	 * Device configuration module handler (dynamic library handler returned by
	 * dlopen
	 */
	void *handler;
	/**
	 * Pointer to mandatory device module function "get_state_data"
	 */
	char * (*get_state_data)(const char * model, const char * running, struct nc_err ** e);
	/**
	 * Pointer to mandatory device module function "execute_operation"
	 */
	nc_reply * (*execute_operation)(const struct nc_session *, const nc_rpc *);
	/**
	 * Pointer to mandatory device module function "init_plugin"
	 */
	char * (*init_plugin)(int, nc_reply * (*device_process_rpc)(int, const struct nc_session*, const nc_rpc*), const char*);
	/**
	 * Pointer to mandatory device module function "close_plugin"
	 */
	int (*close_plugin)(void);
};

struct server_module_list {
	struct server_module *dev;
	struct server_module_list *next;
	struct server_module_list *prev;
};

/**
 * @ingroup dbus
 * @brief Set new NETCONF session connected to the server via Netopeer agent and
 * its DBus connection. The function sends reply to the Netopeer agent.
 *
 * @param conn DBus connection to the Netopeer agent
 * @param msg SetSessionParams DBus message from the Netopeer agent
 */
void set_new_session (DBusConnection *conn, DBusMessage *msg);

/**
 * @ingroup dbus
 * @brief Provide agent with list of capabilities currently supported by server.
 *
 * @param conn DBus connection to the Netopeer agent
 * @param msg SetSessionParams DBus message from the Netopeer agent
 */
void get_capabilities (DBusConnection *conn, DBusMessage *msg);

/**
 * @ingroup dbus
 * @brief Perform NETCONF <close-session> operation requested by client via
 * Netopeer agent. This function do not require any reply sent to the agent.
 *
 * @param conn DBus connection to the Netopeer agent
 * @param msg KillSession DBus message from the Netopeer agent
 */
void close_session (DBusConnection *conn, DBusMessage *msg);

/**
 * @ingroup dbus
 * @brief Perform NETCONF <kill-session> operation requested by client via
 * Netopeer agent. The function sends reply to the Netopeer agent.
 *
 * @param conn DBus connection to the Netopeer agent
 * @param msg KillSession DBus message from the Netopeer agent
 */
void kill_session (DBusConnection *conn, DBusMessage *msg);

/**
 * @ingroup dbus
 * @brief Take care of all others NETCONF operation. Based on namespace
 * associated to operation decide to which device module pass the message.
 *
 * @param conn DBus connection to the Netopeer agent
 * @param msg DBus message from the Netopeer agent
 */
void process_operation (DBusConnection *conn, DBusMessage *msg);

/**
 * @ingroup modules
 * @brief Load device module according to the specification in the
 * Netopeer server internal configuration
 *
 * @param[in] name Name of the device module as specified in the internal
 * configuration file.
 * @param[in] device_cfg Device part of internal configuration file.
 * @return 0 on success, non-zero else
 */
int server_modules_add (char *name, xmlNodePtr device_cfg);

/**
 * @ingroup modules
 * @brief Initialize loaded device module and allow its operation
 * @param name Name of loaded device module
 *
 * @return EXIT_SUCCESS or EXIT_FAILURE
 */
int server_modules_allow (char *name);

/**
 * @ingroup modules
 * @brief Remove device with specified name as defined in internal configuration
 * file of the Netopeer server.
 *
 * @param[in] name name of the device module as defined in internal
 * configuration file of the Netopeer server.
 *
 * @return 0 on success, non-zero on error
 */
int server_modules_remove (char* name);

/**
 * @brief Free all session info structures.
 */
void server_sessions_destroy_all (void);

/**
 * @brief Parse internal configuration and set server according it.
 *
 * @param[in] internal_cfg	XML document containing server configuration
 *
 * @return EXIT_SUCCESS or EXIT_FAILURE
 */
int load_configuration (xmlDocPtr internal_cfg);

/**
 * @brief Allows device module to perform actions on repositories whilst server still has control
 *
 * @param[in] dmid Device module ID. Allow accessing the right datastore and hide datastore ID.
 * @param[in] session Session under which apply RPC
 * @param[in] rpc RPC that will be applied
 *
 * @return nc_reply with response to rpc
 */
nc_reply * device_process_rpc (int dmid, const struct nc_session * session, const nc_rpc * rpc);

/**
 * @brief Apply rpc to all device modules which qualify for it
 *
 * @param[in] session Session that sends rpc
 * @param[in] rpc RPC to apply
 *
 * @return nc_reply with response to rpc
 */
nc_reply * server_process_rpc (struct nc_session * session, const nc_rpc * rpc);

/* server session management functions */

/**
 * @brief Returns constant pointer to session info structure specified by session id
 *
 * @param session_id Key for searching
 *
 * @return Constant pointer to session info structure or NULL on error
 */
const struct session_info* server_sessions_get_by_id (const char* session_id);

/**
 * @brief Add session to server internal list
 *
 * @param[in] session_id Assigned session ID
 * @param[in] username Name of user owning the session
 * @param[in] cpblts List of capabilities session supports
 * @param[in] dbus_is D-BUS identification of agent providing communication for session
 */
void server_sessions_add (const char * session_id, const char * username, struct nc_cpblts * cpblts, const char * dbus_id);

/**
 * @brief Close and remove session and stop agent
 *
 * @param session Session to stop.
 * @param msg Human readable reason for SSH session disconnection.
 */
void server_sessions_stop (struct session_info *session, NC_SESSION_TERM_REASON msg);

/**
 * @brief Close and remove all sessions
 */
void server_sessions_destroy_all (void);

/**
 * @brief Returns constant pointer to session info structure specified by D-BUS id
 *
 * @param session_dbus_id Key for searching
 *
 * @return Constant pointer to session info structure or NULL on error
 */
const struct session_info* server_sessions_get_by_dbusid (const char* session_dbus_id);

/* server device module handling function */

/**
 * @ingroup modules
 * @brief Destroy list holding device modules
 *
 * Used in 2 cases:
 * 1 - Free list returned by server_modules_get_*_list function. These function create lists that contain only
 * 		references to devices a thus in this case devices are preserved.
 * 2 - Free internal server list. In this case ALL device modules are freed too.
 *
 * @param remove If remove is not NULL, specified list is removed but device are not.
 * 		If remove is NULL, internal server list of device modules is freed including devices it contain.
 */
void server_modules_free_list (struct server_module_list * remove);

/**
 * @ingroup modules
 * @brief Add device module to server internal list
 *
 * @param[in] name Name of module as will be stored in device module structure
 * @param[in] device_cfg Pointer to device module part of configuration.
 *
 * @return EXIT_SUCCESS or EXIT_FAILURE
 */
int server_modules_add (char *name, xmlNodePtr device_cfg);

/**
 * @ingroup
 * @brief Find device module using device module id as a key
 *
 * @param[in] id Key ID.
 *
 * @return Pointer to found device module or NULL in case device module was not found.
 */
const struct server_module * server_modules_get_by_dmid (int id);

/**
 * @brief Get pointer to the device module information structure in the
 * internal list. The device module is specified by datastore id.
 *
 * @param id libnetconf datastore ID.
 *
 * @return Device module information structure or NULL if no such device exists.
 */
const struct server_module* server_modules_get_by_repoid (ncds_id id);

/**
 * @ingroup
 * @brief Find device module using device module name as a key
 *
 * @param[in] name Key ID.
 *
 * @return Pointer to found device module or NULL in case device module was not found.
 */
const struct server_module * server_modules_get_by_name(const char * name);

/**
 * @ingroup modules
 * @brief Find all devices that implement rpc operation and support at least one of capability
 *
 * @param[in] rpc RPC message, containing operation to look for.
 * @param[in] cpblts Structure holding list of capabilities.
 *
 * @return Pointer to first member of list or NULL if none such device module found.
 */
struct server_module_list * server_modules_get_providing_rpc_list (const nc_rpc * rpc);

/**
 * @ingroup modules
 * @brief Return complete list of device modules loaded.
 *
 * @return Pointer to first member of list or NULL if none device module loaded.
 */
struct server_module_list * server_modules_get_all ();

/**
 * @brief Print verbose message to syslog
 *
 */
void clb_print (NC_VERB_LEVEL, const char *);

#endif