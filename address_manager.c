#include "address_manager.h"
#include "avltree.h"
#include "log.h"
#include "socket.h"
#include "worker.h"
#include <limits.h>

#define BITMASK(b)     (1 << ((b) % CHAR_BIT))
#define BITSLOT(b)     ((b) / CHAR_BIT)
#define BITSET(a, b)   ((a)[BITSLOT(b)] |= BITMASK(b))
#define BITCLEAR(a, b) ((a)[BITSLOT(b)] &= ~BITMASK(b))
#define BITTEST(a, b)  ((a)[BITSLOT(b)] & BITMASK(b))
#define BITNSLOTS(nb)  ((nb + CHAR_BIT - 1) / CHAR_BIT)

#define NPORTS (32768 - 1024)

struct avl_tree_t address_map;
static char used_ports_bitset[BITNSLOTS(NPORTS)];

void address_mngr_init()
{
    avl_tree_construct(&address_map);

    memset(used_ports_bitset, 0, BITNSLOTS(NPORTS));
}

static int create_unique_port_from_ip(const char *address)
{
    const char *it = address;
    int bytes[4] = { 0 }, idx = 0, byte3, byte4, first_part, offset;

    while (*it != '\0') {
        if (isdigit(*it)) {
            bytes[idx] *= 10;
            bytes[idx] += *it - '0';
        } else
            ++idx;
        ++it;
    }

    byte3 = bytes[2];
    byte4 = bytes[3];

    first_part = byte3;
    if (byte3 >= 100 && byte4 >= 100)
        first_part = byte3 % 100;
    if (byte3 < 10)
        first_part = byte3 * 10;

    offset = byte4 < 100 ? 100 : 1000;

    return first_part * offset + byte4;
}

void address_mngr_add_address_by_port(int port, const char *address)
{
    int fd;
    struct soap_instance *instance;

    if (BITTEST(used_ports_bitset, port - 1024))
        return;

    BITSET(used_ports_bitset, port - 1024);

    fd = socket_create_udp(port);

    instance = soap_instance_allocate(address);
    if (instance == NULL)
        return;

    worker_add_udp_fd(fd);

    log("add address map fd %d -> port %d -> address %s", fd, port, address);
    avl_tree_insert(&address_map, fd, instance);

    soap_instance_print_info(instance);

    log(" ");
}

void address_mngr_add_address(const char *address)
{
    int port = create_unique_port_from_ip(address);

    log("address manager: address '%s' assigned port %d", address, port);

    address_mngr_add_address_by_port(port, address);
}

struct soap_instance* address_mngr_get_soap_instance_from_fd(int fd)
{
    struct soap_instance* instance = avl_tree_find(&address_map, fd);
    if (instance == NULL)
        die(ERR_SOCKET, "address manager: failed to find fd = %d", fd);

    return instance;
}

static struct soap_instance* find_soap_instance_matching_ip(struct avl_node_t *node, const char *ip)
{
    if (node == NULL)
        return NULL;

    struct soap_instance *res, *data;

    if ((res = find_soap_instance_matching_ip(node->left, ip)) != NULL)
        return res;

    if ((res = find_soap_instance_matching_ip(node->right, ip)) != NULL)
        return res;

    data = node->data;

    if (strstr(data->service_endpoint, ip) != NULL)
        return data;

    return NULL;
}

struct soap_instance* address_mngr_find_soap_instance_matching_ip(const char *ip)
{
    if (address_map.root == NULL)
        return NULL;

    return find_soap_instance_matching_ip(address_map.root, ip);
}

static void node_destruction_cb(struct avl_node_t *node)
{
    close(node->key);
    soap_instance_deallocate(node->data);
}

void address_mngr_destruct()
{
    avl_tree_destruct(&address_map, node_destruction_cb);
}

