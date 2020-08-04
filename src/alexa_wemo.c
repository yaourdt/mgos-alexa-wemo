#include "mgos.h"
#include "mongoose.h"
#include "mgos_sys_config.h"
#include "mgos_mdns_internal.h"
#include "mgos_http_server.h"

#define SSDP_MCAST_GROUP "239.255.255.250"
#define SSDP_LISTEN_ADDR "udp://:1900"

// *** global service config
typedef struct {
	int  nextId;
	int  nextSend;
	bool send;
	char ip[17];
} alexa_wemo_service_config;
alexa_wemo_service_config *wemo_config;

// *** config per service entry
struct alexa_wemo_service_entry {
	int  id;
	int  gpio_pin;
	char *name;
	char usn[15];
	char uuid[37];
	SLIST_ENTRY(alexa_wemo_service_entry) next;
};
SLIST_HEAD(wemo_instances, alexa_wemo_service_entry) wemo_instances;

// *** send SSDP response
static void alexa_wemo_udp_in_ev(struct mg_connection *nc, int ev, void *ev_data, void *user_data) {
	struct alexa_wemo_service_entry *e = NULL;

	switch (ev) {
	case MG_EV_ACCEPT:
		wemo_config->send = false;
		break;
	case MG_EV_RECV:
		if ( mg_strstr(mg_mk_str(nc->recv_mbuf.buf), mg_mk_str("M-SEARCH") ) != NULL
		&&   mg_strstr(mg_mk_str(nc->recv_mbuf.buf), mg_mk_str("MAN: \"ssdp:discover\"") ) != NULL ) {
			wemo_config->send     = true;
			wemo_config->nextSend = 0   ;
			nc->flags &= ~MG_F_SEND_AND_CLOSE; // do not close connection at this point
		} else {
			wemo_config->nextSend = wemo_config->nextId;
			mbuf_remove(&nc->recv_mbuf, nc->recv_mbuf.len);
			nc->flags |= MG_F_CLOSE_IMMEDIATELY;
		}
		break;
	case MG_EV_POLL:
		if ( wemo_config->nextSend == wemo_config->nextId ) {
			mbuf_remove(&nc->recv_mbuf, nc->recv_mbuf.len);
			break;
		}
		if ( !wemo_config->send ) { break; }

		SLIST_FOREACH(e, &wemo_instances, next) {
			if ( e->id == wemo_config->nextSend ) {
				mg_printf(nc, "HTTP/1.1 200 OK\r\n");
				mg_printf(nc, "CACHE-CONTROL: max-age=86400\r\n");
				mg_printf(nc, "DATE: Fri, 14 Aug 2020 02:42:00 GMT\r\n");
				mg_printf(nc, "EXT:\r\n");
				mg_printf(nc, "LOCATION: http://%s:%i/setup.xml\r\n", wemo_config->ip, e->id + mgos_sys_config_get_alexa_wemo_port() );
				mg_printf(nc, "OPT: \"http://schemas.upnp.org/upnp/1/0/\"; ns=01\r\n");
				mg_printf(nc, "01-NLS: %s\r\n", e->uuid);
				mg_printf(nc, "SERVER: Unspecified, UPnP/1.0, Unspecified\r\n");
				if ( mg_strstr(mg_mk_str(nc->recv_mbuf.buf), mg_mk_str("urn:Belkin:device:**")) != NULL ) {
					mg_printf(nc, "ST: urn:Belkin:device:**\r\n");
					mg_printf(nc, "USN: uuid:Socket-1_0-%s::urn:Belkin:device:**\r\n", e->usn);
				} else {
					mg_printf(nc, "ST: upnp:rootdevice\r\n");
					mg_printf(nc, "USN: uuid:Socket-1_0-%s::upnp:rootdevice\r\n", e->usn);
				}
				mg_printf(nc, "X-User-Agent: redsonic\r\n");
				mg_printf(nc, "\r\n");
				wemo_config->send = false;
				wemo_config->nextSend = wemo_config->nextSend + 1;
			}
		}
		break;
	case MG_EV_SEND:
		wemo_config->send = true;
		break;
	}
	(void) ev_data;
	(void) user_data;
}

// *** handle HTTP requests, one port per service
static void alexa_wemo_http_in_ev(struct mg_connection *nc, int ev, void *ev_data, void *user_data) {
	if (ev != MG_EV_HTTP_REQUEST) { return; }
	struct http_message *hm = (struct http_message *) ev_data;
	struct alexa_wemo_service_entry *e = (struct alexa_wemo_service_entry *) user_data;

	if ( mg_vcmp( &hm->uri, "/setup.xml") == 0 ) {
		mg_printf(nc, "HTTP/1.1 200 OK\r\n");
		mg_printf(nc, "CONTENT-TYPE: text/xml; charset=\"utf-8\"\r\n");
		mg_printf(nc, "\r\n");
		mg_printf(nc, "<?xml version=\"1.0\"?>");
		mg_printf(nc, "<root><device>");
		mg_printf(nc, "<deviceType>urn:%s:device:controllee:1</deviceType>", mgos_sys_config_get_alexa_wemo_vendor() );
		mg_printf(nc, "<friendlyName>%s</friendlyName>", e->name);
		mg_printf(nc, "<manufacturer>Belkin International Inc.</manufacturer>");
		mg_printf(nc, "<modelName>%s %s</modelName>", mgos_sys_config_get_alexa_wemo_vendor(), mgos_sys_config_get_alexa_wemo_model() );
		mg_printf(nc, "<modelNumber>1.0</modelNumber>");
		mg_printf(nc, "<modelDescription>%s</modelDescription>", mgos_sys_config_get_alexa_wemo_description() );
		mg_printf(nc, "<UDN>uuid:Socket-1_0-%s</UDN>", e->usn);
		mg_printf(nc, "</device></root>\r\n");
	} else if ( mg_vcmp( &hm->uri, "/upnp/control/basicevent1") == 0 ) {
		mg_printf(nc, "HTTP/1.1 200 OK\r\n");
		mg_printf(nc, "CONTENT-TYPE: text/xml; charset=\"utf-8\"\r\n");
		mg_printf(nc, "\r\n");
		mg_printf(nc, "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body>");
		if ( mg_strstr(hm->body, mg_mk_str("SetBinaryState") ) != NULL ) {
			mg_printf(nc, "<u:SetBinaryStateResponse xmlns:u=\"urn:Belkin:service:basicevent:1\">");
			if ( mg_strstr(hm->body, mg_mk_str("<BinaryState>1</BinaryState>") ) != NULL ) {
				mgos_gpio_write(e->gpio_pin, 1);
				mg_printf(nc, "<BinaryState>1</BinaryState>");
			} else {
				mgos_gpio_write(e->gpio_pin, 0);
				mg_printf(nc, "<BinaryState>0</BinaryState>");
			}
			mg_printf(nc, "</u:SetBinaryStateResponse>");
		} else {
			mg_printf(nc, "<u:GetBinaryStateResponse xmlns:u=\"urn:Belkin:service:basicevent:1\">");
			mg_printf(nc, "<BinaryState>%i</BinaryState>", mgos_gpio_read(e->gpio_pin));
			mg_printf(nc, "</u:GetBinaryStateResponse>");
		}
		mg_printf(nc, "</s:Body></s:Envelope>\r\n");
	} else {
		mg_printf(nc, "HTTP/1.0 404 Not Found\r\n");
		mg_printf(nc, "CONTENT-LENGTH: 0\r\n");
		mg_printf(nc, "\r\n");
	}
	nc->flags |= MG_F_SEND_AND_CLOSE;
}

// *** add a new virtual switch on a dedicated port
bool alexa_wemo_add_instance(char *name, int gpio_pin) {
	bool success = false;
	char port[6];
	if ( !mgos_sys_config_get_alexa_wemo_enable() ) { success = true; goto out; }
	if ( wemo_config->nextId > 15 ) { LOG(LL_ERROR, ("Max. number of instances reached")); goto out; }

	struct alexa_wemo_service_entry *e;
	if ((e = calloc(1, sizeof(*e))) == NULL) { LOG(LL_ERROR, ("Out of memory")); goto out; }
	const char *mac = mgos_sys_ro_vars_get_mac_address();
	sprintf(e->uuid, "A2DAE02C-9E41-4856-ABF%x-%s", wemo_config->nextId, mac);
	sprintf(e->usn , "%s-%x%s", mgos_sys_config_get_alexa_wemo_vendor(), wemo_config->nextId, mac + 6);
	LOG(LL_DEBUG, ("UUID %s and USN %s generated.", e->uuid, e->usn));
	e->name     = name;
	e->gpio_pin = gpio_pin;
	e->id       = wemo_config->nextId;

	sprintf(port, ":%i", e->id + mgos_sys_config_get_alexa_wemo_port() );
	struct mg_connection *nc = mg_bind(mgos_get_mgr(), port, alexa_wemo_http_in_ev, e);
	if (nc == NULL) { LOG(LL_ERROR, ("Failed to bind to tcp port %s", port)); goto out; }
	mg_set_protocol_http_websocket(nc);

	wemo_config->nextId = wemo_config->nextId + 1;
	SLIST_INSERT_HEAD(&wemo_instances, e, next);
	success = true;
	LOG(LL_INFO, ("Wemo instance %s added, listening on tcp port %s", name, port));
out:
	return success;
}

// *** handler to determine ip address
static void alexa_wemo_net_ev_handler(int ev, void *evd, void *arg) {
	if ( ev != MGOS_NET_EV_IP_ACQUIRED ) { return; }

	struct mgos_net_ip_info ip_info;
	if (mgos_net_get_ip_info(MGOS_NET_IF_TYPE_WIFI, MGOS_NET_IF_WIFI_STA, &ip_info)) {
		mgos_net_ip_to_str(&ip_info.ip, wemo_config->ip);
	}

	LOG(LL_DEBUG, ("Joining SSDP multicast group %s", SSDP_MCAST_GROUP));
	if ( !mgos_mdns_hal_join_group(SSDP_MCAST_GROUP) ) { return; }

	struct mg_connection *nc = mg_bind(mgos_get_mgr(), SSDP_LISTEN_ADDR, alexa_wemo_udp_in_ev, NULL);
	if (nc == NULL) { LOG(LL_ERROR, ("Failed to bind to %s", SSDP_LISTEN_ADDR)); return; }
	LOG(LL_INFO, ("Wemo emulation started. Listening on %s", SSDP_LISTEN_ADDR));

	(void) evd;
	(void) arg;
}

// *** init lib
bool mgos_mgos_alexa_wemo_init(void) {
	bool success = false;
	if ( !mgos_sys_config_get_alexa_wemo_enable() ) { success = true; goto out; }

	if ((wemo_config = calloc(1, sizeof(*wemo_config))) == NULL) { LOG(LL_ERROR, ("Out of memory")); goto out; }
	wemo_config->nextId = 0;
	mgos_event_add_group_handler(MGOS_EVENT_GRP_NET, alexa_wemo_net_ev_handler, NULL);

	success = true;
out:
	return success;
}
