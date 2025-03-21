#include "csp_server.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>

#include <csp/csp.h>
#include <csp/interfaces/csp_if_zmqhub.h>
#include <param/param_server.h>
#include <vmem/vmem_file.h>
#include <vmem/vmem_server.h>

#include "vmem_config.h"
#include "param_config.h"
#include <csp/interfaces/csp_if_can.h>
#include <csp/interfaces/csp_if_kiss.h>
#include <csp/drivers/usart.h>
#include <csp/drivers/can_socketcan.h>
#include <csp/drivers/usart.h>
#include <sys/types.h>
#include <errors.hpp>

// shared resources and mutexes
char capture_instruction[PARAM_MAX_SIZE];
pthread_mutex_t mutex;
pthread_cond_t cond;

// For catching ctrl-c
static volatile int _RUNNING = 1;
void intHandler(int _) {
    _RUNNING = 0;
    pthread_cond_signal(&cond);
}

void* vmem_server_task(void* param) {
    vmem_server_loop(param);
    return NULL;
}

void* router_task(void* param) {
    while (1) {
        csp_route_work();
    }
    return NULL;
}

void capture_param_callback(struct param_s *param, int offset) {
    char* data = malloc(PARAM_MAX_SIZE);
    param_get_string(param, data, PARAM_MAX_SIZE);
    pthread_mutex_lock(&mutex);
    
    strcpy(capture_instruction, data);
    
    pthread_cond_signal(&cond);
    pthread_mutex_unlock(&mutex);
    free(data);
}

static void csp_init_fun(void) {
    csp_conf.hostname = "Camera";
    csp_conf.model = "DISCO-II";
    csp_conf.revision = "1";
	csp_conf.version = 2;
	csp_conf.dedup = CSP_DEDUP_OFF;

    csp_init();

    csp_bind_callback(csp_service_handler, CSP_ANY);
    csp_bind_callback(param_serve, PARAM_PORT_SERVER);

    static pthread_t vmem_server_handle;
    pthread_create(&vmem_server_handle, NULL, &vmem_server_task, NULL);

    static pthread_t router_handle;
    pthread_create(&router_handle, NULL, &router_task, NULL);

    // static pthread_t onehz_handle;
    // pthread_create(&onehz_handle, NULL, &onehz_task, NULL);
}

/// @brief Initialize communication interfaces: ZMQ, CAN and KISS
/// @param interfaceConfig configuration parameters
static void iface_init(CSPInterface *interfaceConfig) {
    int error = CSP_ERR_NONE;
    csp_iface_t * default_iface = NULL;

    switch (interfaceConfig->Interface)
    {
    case ZMQ:
        error = csp_zmqhub_init_filter2("zmq", interfaceConfig->Device, interfaceConfig->Node, 8, true, &default_iface, NULL, CSP_ZMQPROXY_SUBSCRIBE_PORT, CSP_ZMQPROXY_PUBLISH_PORT);
        default_iface->name = "zmq";
        break;
    // case CAN:
	//     error = csp_can_socketcan_open_and_add_interface(interfaceConfig->Device, "CAN", interfaceConfig->Node, 1000000, 0, &iface);
    //     default_iface->name = "CAN";
    //     break;
    case KISS:
        csp_usart_conf_t conf = {
            .device = interfaceConfig->Device,
            .baudrate = 115200, /* supported on all platforms */
            .databits = 8,
            .stopbits = 1,
            .paritysetting = 0,
        };
        error = csp_usart_open_and_add_kiss_interface(&conf, CSP_IF_KISS_DEFAULT_NAME, interfaceConfig->Node,  &default_iface);
        default_iface->addr = interfaceConfig->Node;
        default_iface->name = "kiss";
        break;
    }

    if (error != CSP_ERR_NONE) {
        csp_print("failed to add interface [%s], error: %d\n", interfaceConfig->Device, error);
        exit(1);
    }

    default_iface->is_default = 1;
    default_iface->addr = interfaceConfig->Node;
	default_iface->netmask = 8;
	csp_rtable_set(0, 0, default_iface, CSP_NO_VIA_ADDRESS);
	csp_iflist_add(default_iface);
}

void server_start(CSPInterface *interfaceConfig, CallbackFunc callback, void* obj) {
    pthread_mutex_init(&mutex, NULL);
    pthread_cond_init(&cond, NULL);

    void serial_init(void);
    serial_init();

    // Parameter storage
    vmem_file_init(&vmem_config);
    
    // Interfaces
    iface_init(interfaceConfig);

    // Initialize CSP
    csp_init_fun();
    param_set_string(&capture_param, "", PARAM_MAX_SIZE);

    signal(SIGINT, intHandler);
    while (_RUNNING) {
        pthread_mutex_lock(&mutex);
        pthread_cond_wait(&cond, &mutex);
        u_int16_t error = 0;

        if(strlen(capture_instruction) > 0 && _RUNNING){
            callback(capture_instruction, obj, &error);
        }
        pthread_mutex_unlock(&mutex);
        param_set_uint16(&error_log, error);
    }
}
