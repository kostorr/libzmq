#ifndef __ZMQ_RIO_HPP_INCLUDED__
#define __ZMQ_RIO_HPP_INCLUDED__

#if defined (ZMQ_HAVE_RIO)

#include <rapidio_mport_dma.h>
#include <rapidio_mport_mgmt.h>
#include <rapidio_mport_sock.h>

#include <iostream>
#include <cstring>
#include <string>
#include <map>
#include <vector>
#include <algorithm>

/* Reserved memory areas for the quads
 *  * NOTE: Make sure they are in line with what
 *   * the OS has allocated on boot
 *    */
#define TRIOSYS_DRV_DMA_QUADS_RES_MEM_ADDR (0x140000000)
#define TRIOSYS_DRV_DMA_QUADS_RES_MEM_SIZE (0x40000000)

/* Reserved memory areas for the PC cluster.
 *  * NOTE: Make sure they are in line with what
 *   * the OS has allocated on boot
 *    */
#define TRIOSYS_DRV_DMA_PCC_RES_MEM_ADDR (0x50000000)
#define TRIOSYS_DRV_DMA_PCC_RES_MEM_SIZE (0x10000000)
//#define TRIOSYS_DRV_DMA_PCC_RES_MEM_SIZE (0xA000)


/* Change these depending on the cluster on which ZeroMQ is running */
#define TRIOSYS_DRV_DMA_RES_MEM_ADDR (TRIOSYS_DRV_DMA_PCC_RES_MEM_ADDR)
#define TRIOSYS_DRV_DMA_RES_MEM_SIZE (TRIOSYS_DRV_DMA_PCC_RES_MEM_SIZE)

#define RIO_DMA_BUF_NUM (32)
#define NUM_CONNS (1)

struct rio_conn_info {

    riomp_mport_t *mport_hnd;
    uint64_t *dma_mem_hnd;
    uint32_t local_dest_id;
    uint32_t remote_dest_id;
    uint64_t tx_tgt_addr;
    int conn_id;
    void *rx_buf;
};

// mport_hnd needs to be allocated by caller...
static inline int RapidIODMAPrep (riomp_mport_t *mport_hnd, uint64_t *rx_tgt_addr, uint64_t *dma_mem_hnd, void **rx_buf) {
     
    // --- INITIALIZE DMA ---

    struct riomp_mgmt_mport_properties prop;
    int ret = 0;
    int mport_id = 0;

    ret = riomp_mgmt_mport_create_handle (mport_id, 0, mport_hnd);
    if (ret) {
        std::cerr << "Failed to create mport handle";
        return -1;
    }

    //printf("mport_hnd is 0x%lx\n", mport_hnd);

    //check link speed and availability
    if (!riomp_mgmt_query (*mport_hnd, &prop)) {
        riomp_mgmt_display_info(&prop);

        if (prop.link_speed == 0){
            riomp_mgmt_mport_destroy_handle (mport_hnd);
            return -1;
        }
    }

    //   --- MAP DMA MEMORY ---

    uint32_t dma_mem_size = TRIOSYS_DRV_DMA_RES_MEM_SIZE;

    *dma_mem_hnd = TRIOSYS_DRV_DMA_RES_MEM_ADDR;
    *rx_tgt_addr = RIOMP_MAP_ANY_ADDR;

    ret = riomp_dma_ibwin_map (*mport_hnd, rx_tgt_addr, dma_mem_size, dma_mem_hnd);
    if (ret) {
        std::cerr << "ibwin_map err" << ret;
        riomp_mgmt_mport_destroy_handle (mport_hnd);
        return -1;
    }

    ret = riomp_dma_map_memory (*mport_hnd, dma_mem_size, *dma_mem_hnd, rx_buf);
    if (ret) {
        std::cerr << "mmap err" << ret;
        return -1;
    }

    return 0;
}

static inline int RapidIODMAHandshakeListener (riomp_sock_t rio_socket, uint64_t rx_tgt_addr, uint64_t *tx_tgt_addr,
        int conn_id, rapidio_mport_socket_msg *msg_tx_buf, rapidio_mport_socket_msg *msg_rx_buf) {

    // --- GET REMOTE TARGET ADDRESS ---

    char handshake_buf[100];

    msg_rx_buf = (rapidio_mport_socket_msg *) calloc (1, sizeof(rapidio_mport_socket_msg));
    if (!msg_rx_buf){
        std::cerr << "failed to calloc msg_rx_buf";
        return -1;
    }

    int ret = riomp_sock_receive (rio_socket, &(msg_rx_buf),
            0, 0);
    if (ret) {
        std::cerr << "riomp_sock_receive, err=" << ret;
        return -1;
    }

    sscanf((char *) msg_rx_buf->msg.payload, "HANDSHAKE|%lx", tx_tgt_addr);

    // --- SEND LOCAL TARGET ADDRESS ---

    sprintf(handshake_buf, "HANDSHAKE|%lx", rx_tgt_addr);

    ret = riomp_sock_request_send_buffer(rio_socket, &msg_tx_buf);
    if (ret) {
        std::cerr << "riomp_sock_request_send_buffer, err=" << ret;
        return -1;
    }

    memcpy (msg_tx_buf->msg.payload, handshake_buf, strlen(handshake_buf));

    ret = riomp_sock_send (rio_socket, msg_tx_buf, sizeof(*msg_tx_buf), NULL);
    if (ret) {
        std::cerr << "riomp_sock_send, err=" << ret;
        return -1;
    }
    
    memset(msg_tx_buf, 0, sizeof(rapidio_mport_socket_msg));

    // --- SEND CONNECTION ID --- //

    sprintf (handshake_buf, "HANDSHAKE|CONNID|%d", conn_id);
    memcpy (msg_tx_buf->msg.payload, handshake_buf, strlen(handshake_buf));

    ret = riomp_sock_send (rio_socket, msg_tx_buf, sizeof(*msg_tx_buf), NULL);
    if (ret) {
        std::cerr << "riomp_sock_send, err=" << ret;
        return -1;
    }

    return 0;
}

static inline int RapidIODMAHandshakeConnecter (riomp_sock_t rio_socket, uint64_t rx_tgt_addr, uint64_t *tx_tgt_addr,
        int *conn_id, rapidio_mport_socket_msg *msg_tx_buf, rapidio_mport_socket_msg *msg_rx_buf) {

    // --- SEND LOCAL TARGET ADDRESS ---

    int rc = riomp_sock_request_send_buffer(rio_socket, &msg_tx_buf);
    if (rc) {
        std::cerr << "riomp_sock_request_send_buffer, err=" << rc;
        return -1;
    }

    char handshake_buf[100];
    sprintf(handshake_buf, "HANDSHAKE|%lx", rx_tgt_addr);

    memcpy (msg_tx_buf->msg.payload, handshake_buf, strlen(handshake_buf));

    rc = riomp_sock_send (rio_socket, msg_tx_buf, sizeof(*msg_tx_buf), NULL);
    if (rc) {
        std::cerr << "riomp_sock_send, err=" << rc;
        return -1;
    }

    // --- GET REMOTE TARGET ADDRESS ---

    if (!msg_rx_buf){
        msg_rx_buf = (rapidio_mport_socket_msg *) calloc (1, sizeof(rapidio_mport_socket_msg));
        if (!msg_rx_buf){
            std::cerr << "failed to calloc msg_rx_buf";
            return -1;
        }
    }

    rc = riomp_sock_receive (rio_socket, &(msg_rx_buf),
            0, 0);
    if (rc) {
        std::cerr << "riomp_sock_receive, err=" << rc;
        return -1;
    }

    sscanf((char *) msg_rx_buf->msg.payload, "HANDSHAKE|%lx", tx_tgt_addr);

    memset(msg_rx_buf, 0, sizeof(rapidio_mport_socket_msg));

    // --- GET CONNECTION ID ---
    
    rc = riomp_sock_receive (rio_socket, &(msg_rx_buf),
            0, 0);
    if (rc) {
        std::cerr << "riomp_sock_receive, err=" << rc;
        return -1;
    }

    sscanf((char *) msg_rx_buf->msg.payload, "HANDSHAKE|CONNID|%d", conn_id);

    // Free the tx buffer
    free (msg_tx_buf);

    // Free the rx buffer
    free (msg_rx_buf);

    // Close the RapidIO socket
    if(rio_socket){
        rc = riomp_sock_close(&rio_socket);
        if (rc)
            std::cerr << "riomp_sock_close error" << std::endl;
    }
    return 0;
}

static inline void RapidIODestroyMailbox(riomp_mailbox_t *mailbox) {

    // Release mailbox
    int rc = riomp_sock_mbox_destroy_handle (mailbox);
    if (rc){
        std::cerr << "riomp_sock_mbox_destroy_handle, err=" << rc;
    }

    return;
}

static inline void RapidIOReleaseDMAMemory (riomp_mport_t *mport_hnd, uint64_t *dma_mem_hnd, void *tx_buf) {

    int rc = 0;

    // Free and unmap the inbound window
    if(dma_mem_hnd){
        rc = riomp_dma_ibwin_free (*mport_hnd, dma_mem_hnd);
        if (rc)
            std::cerr << "riomp_dma_ibwin_free" << std::endl;
    }
    
    // Free dma_mem_hnd
    free(dma_mem_hnd);

    // Free tx_buf
    free(tx_buf);

    // Destroy the mport handle
    if(*mport_hnd){
        rc = riomp_mgmt_mport_destroy_handle (mport_hnd);
        if (rc)
            std::cerr << "riomp_mgmt_mport_destroy_handle error" << std::endl;
    }
}

static inline uint32_t RapidIOGetLocalDestid ()
{
    FILE *fp;
    char parse_buf[10];
    uint32_t did = 0xffffffff;

    fp = popen("/bin/cat /sys/class/rio_mport/rio_mport0/device/port_destid", "r");
    if (fp == NULL) {
        printf("Failed to run command\n" );
        exit(1);
    }

    int i = 0;
    char c;
    while((c = fgetc(fp)) != EOF) {
        parse_buf[i] = c;
        i++;
    }
    parse_buf[i] = '\0';

    did = (uint32_t)strtol(parse_buf, NULL, 16);

    pclose(fp);
    return did;
}

static inline std::map<std::pair<int, int>, int> RapidIODBRangeMap (did_val_t *destids, int number_of_eps, uint32_t local_destid){

    std::vector<int> destid_vect;

    auto it = destid_vect.begin();
    for (int i=0; i<number_of_eps; i++)
        it = destid_vect.insert(it, destids[i]);
    it = destid_vect.insert(it, local_destid);

    std::sort (destid_vect.begin(), destid_vect.end());

    std::map <std::pair<int, int>, int> bases;

    int base = 1; //=RIO_CONNECT + 1
    for(int i=0; i<number_of_eps+1; i++)
        for (int j=0; j<number_of_eps+1; j++)
            if(i != j){
                bases.insert (std::make_pair(std::make_pair(destid_vect.at(i), destid_vect.at(j)), base + (RIO_DMA_BUF_NUM/NUM_CONNS)));
                // RIO_DMA_BUF_NUM/NUM_CONNS = doorbell range needed = number of dma slots
                base = bases.at(std::make_pair(destid_vect.at(i), destid_vect.at(j)));
            }

    /*for(int i=0; i<number_of_eps+1; i++)
        for (int j=0; j<number_of_eps+1; j++)
            if (i != j)
                std::cout << "bases[" << destid_vect.at(i) << "," << destid_vect.at(j) << "] = " << bases.at(std::make_pair(destid_vect.at(i), destid_vect.at(j))) << std::endl;*/
    
    return bases; 
}


#endif

#endif
