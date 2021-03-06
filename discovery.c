#include "discovery.h"
#include "soap_header.h"
#include "soap_utils.h"
#include <wsddapi.h>

static struct soap *soap_listen;

void discovery_init()
{
    soap_listen = soap_new1(SOAP_IO_UDP);

    soap_listen->connect_flags |= SO_BROADCAST;

    if (!soap_valid_socket(soap_bind(soap_listen, NULL, 0, 1000))) {
        soap_print_fault(soap_listen, stderr);
        soap_die(soap_listen, "failed to bind wsdd listening soap instance");
    }
}

void discovery_do(int milliseconds)
{
    const char *multicast_url = "soap.udp://239.255.255.250:3702";
    const char *type = "", *scope = "onvif://www.onvif.org/";

    log("starting discovery..");

    if (soap_wsdd_Probe(soap_listen, SOAP_WSDD_ADHOC, SOAP_WSDD_TO_TS, multicast_url,
                soap_wsa_rand_uuid(soap_listen), NULL, type, scope, "") != SOAP_OK) {
        soap_print_fault(soap_listen, stderr);
        soap_die(soap_listen, "failed to wsdd probe");
    }

    soap_wsdd_listen(soap_listen, -milliseconds * 1000);

    log("discovery stop");
    log(" ");
}

void discovery_destruct()
{
    soap_destroy(soap_listen);
    soap_end(soap_listen);
    soap_free(soap_listen);
}

