/*
    Copyright (c) 2007-2016 Contributors as noted in the AUTHORS file

    This file is part of libzmq, the ZeroMQ core engine in C++.

    libzmq is free software; you can redistribute it and/or modify it under
    the terms of the GNU Lesser General Public License (LGPL) as published
    by the Free Software Foundation; either version 3 of the License, or
    (at your option) any later version.

    As a special exception, the Contributors give you permission to link
    this library with independent modules to produce an executable,
    regardless of the license terms of these independent modules, and to
    copy and distribute the resulting executable under terms of your choice,
    provided that you also meet, for each linked independent module, the
    terms and conditions of the license of that module. An independent
    module is a module which is not derived from or based on this library.
    If you modify this library, you must extend this exception to your
    version of the library.

    libzmq is distributed in the hope that it will be useful, but WITHOUT
    ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
    FITNESS FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public
    License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "rio_connecter.hpp"

#if defined ZMQ_HAVE_RIO

#include <iostream>
#include "io_thread.hpp"
#include "random.hpp"
#include "err.hpp"
#include "ip.hpp"
#include "address.hpp"
#include "session_base.hpp"
#include "rio_address.hpp"
#include "rio_engine.hpp"
#include "../include/zmq.h"

zmq::rio_connecter_t::rio_connecter_t (class io_thread_t *io_thread_,
        class session_base_t *session_, const options_t &options_,
        const address_t *addr_, bool delayed_start_) :
    own_t (io_thread_, options_),
    io_object_t (io_thread_),
    addr (addr_),
    s (retired_fd),
    handle((handle_t)NULL),
    handle_valid (false),
    delayed_start (delayed_start_),
    timer_started (false),
    session (session_),
    current_reconnect_ivl (options.reconnect_ivl),
    rioh_mailbox(false, "CONNECTER", NULL)
{
    zmq_assert (addr);
    zmq_assert (addr->protocol == "rio");
    addr->to_string (endpoint);
    socket = session->get_socket ();
}

zmq::rio_connecter_t::~rio_connecter_t ()
{
    zmq_assert (!timer_started); //TODO: Is this relevant when reconnect not implemented?
    zmq_assert (!handle_valid); 
    zmq_assert (s == retired_fd);
}
void zmq::rio_connecter_t::process_plug ()
{
    if (delayed_start)
        add_reconnect_timer ();
    else
        start_connecting ();
}

void zmq::rio_connecter_t::process_term (int linger_)
{

    if (timer_started) {
        cancel_timer (reconnect_timer_id);
        timer_started = false;
    }

    if (handle_valid) {
        rm_fd (handle);
        handle_valid = false;
    }

    if (s != retired_fd)
        close ();

    own_t::process_term (linger_);
}

void zmq::rio_connecter_t::in_event()
{
    // We are not polling for incoming data, so we are actually called
    // because of error here. However, we can get error on out event as well
    // on some platforms, so we'll simply handle both events in the same way.
    out_event ();
}

void zmq::rio_connecter_t::out_event () //TODO: This would be "make sure we're connected"
{
    fd_t fd = s;
    s = retired_fd; // Poll shouldn't listen to any more events from here.
                    // So mark this fd as inactive.

    rm_fd(handle);
    handle_valid = false;

    // Make sure we are connected
    uint16_t dest_id = addr->resolved.rio_addr->get_dest_id (); 
    //uint16_t channel = addr->resolved.rio_addr->get_channel ();

    // Initialize DMA
    mport_hnd = (riomp_mport_t *) malloc (sizeof(riomp_mport_t)); //Allocate mport_hnd before calling DMAPrep
    dma_mem_hnd = (uint64_t *) malloc (sizeof(uint64_t)); // Same for dma_mem_hnd
    int rc = RapidIODMAPrep(mport_hnd, &rx_tgt_addr, dma_mem_hnd, &rx_buf);
    if (rc){
        std::cerr << "RapidIODMAPrep failed";
        terminate();
        return;
    }

    // Exchange relevant information with remote end
    int conn_id = -1;
    rc = RapidIODMAHandshakeConnecter(*rio_socket, rx_tgt_addr, &tx_tgt_addr, &conn_id, msg_tx_buf, msg_rx_buf);
    if (rc){
        std::cerr << "RapidIODMAHandshake failed";
        terminate();
        return;
    }
    /*std::cout << "Handshaking done" << std::endl;
    printf("tx_tgt_addr is 0x%lx\n", tx_tgt_addr);
    printf("mport_hnd is 0x%lx\n", mport_hnd);
    printf("conn_id is %d\n", conn_id);*/

    // TODO: Could something like this work with RapidIO? 
    // It would be the reconnect mechanism
    // Send a doorbell to wake up the server
    //rioh_mailbox.send_db(dest_id, RIO_CONNECT);

    /*while (0 != riomp_sock_connect(*rio_socket, dest_id, channel)){
        if (errno == EISCONN){
            break;
        }
        std::cerr << "rio_connect failed " << strerror (errno) << std::endl;
    }*/

    rio_conn_info rci = {};
    rci.mport_hnd = mport_hnd;
    rci.dma_mem_hnd = dma_mem_hnd;
    rci.remote_dest_id = dest_id; //pass the *remote* destination id
    rci.local_dest_id = RapidIOGetLocalDestid(); //pass the *local* destination id
    rci.tx_tgt_addr = tx_tgt_addr;
    rci.rx_buf = rx_buf;
    rci.conn_id = conn_id;

    // Create the engine object for this connection.
    rio_engine_t *engine = new (std::nothrow)
        rio_engine_t (rci, endpoint);
    alloc_assert (engine);

    // Attach the engine to the corresponding session object.
    send_attach (session, engine);

    socket->event_connected (endpoint, (int) fd);

    // Shut the connecter down
    terminate ();
}

void zmq::rio_connecter_t::timer_event (int id_)
{
    zmq_assert (id_ == reconnect_timer_id);
    timer_started = false;
    start_connecting ();
}

void zmq::rio_connecter_t::start_connecting()
{
    // Open the connecting socket
    const int rc = open ();

    // Connect may succeed in synchronous manner.
    if (rc == 0){
        handle = add_fd (s);
        handle_valid = true;
        out_event ();
    }// Connection establishment may be delayed. Poll for its completion.
    else if (rc == -1 && errno == EINPROGRESS) { //Kostas: Not implemented for rio
        handle = add_fd (s);
        handle_valid = true;
        set_pollout (handle);
        socket->event_connect_delayed (endpoint, zmq_errno());
    }// Handle any other condition by eventual reconnect.
    else {
        if (s != retired_fd)
            close ();
        add_reconnect_timer ();
    }
}

void zmq::rio_connecter_t::add_reconnect_timer ()
{
    int rc_ivl = get_new_reconnect_ivl();
    add_timer (rc_ivl, reconnect_timer_id);
    socket->event_connect_retried (endpoint, rc_ivl);
    timer_started = true;
}

int zmq::rio_connecter_t::get_new_reconnect_ivl ()
{
    // The new interval is the current interval + random value..
    int this_interval = current_reconnect_ivl +
        (generate_random () % options.reconnect_ivl);

    // Only change the current reconnect interval if the maximum reconnect
    // interval was set and if it's larger than the reconnect interval.
    if (options.reconnect_ivl_max > 0 &&
            options.reconnect_ivl_max > options.reconnect_ivl){

        // Calculate the next interval
        current_reconnect_ivl = current_reconnect_ivl * 2;
        if (current_reconnect_ivl >= options.reconnect_ivl_max) {
            current_reconnect_ivl = options.reconnect_ivl_max;
        }
    }
    return this_interval;
}

int zmq::rio_connecter_t::open ()
{
    zmq_assert (s == retired_fd);
    
    // Create the socket
    uint32_t mport_id = 0;
    mailbox = (riomp_mailbox_t *)malloc(sizeof(riomp_mailbox_t));
    int rc = riomp_sock_mbox_create_handle (mport_id, 0, mailbox);
    if (rc) {
        std::cerr << "riomp_sock_mbox_create_handle in open() failed " << strerror(errno) << std::endl;
        return -1;
    }
    
    rio_socket = (riomp_sock_t *)malloc(sizeof(riomp_sock_t));
    
    rc = riomp_sock_socket (*mailbox, rio_socket);
    if (rc) {
        std::cerr << "riomp_sock_socket in open() failed " << strerror(errno) << std::endl;
        return -1;
    }

    s = rioh_mailbox.get_fd ();
    unblock_socket (s);

    uint32_t dest_id = addr->resolved.rio_addr->get_dest_id ();
    uint16_t channel = addr->resolved.rio_addr->get_channel ();

    // Send a doorbell to wake up the server
    rioh_mailbox.send_db(dest_id, RIO_CONNECT);

    rc = riomp_sock_connect(*rio_socket, dest_id, channel, NULL);

    // Connect was succesful immediately
    if (rc == 0)
        return 0;

    std::cerr << "riomp_sock_connect in open() failed " << strerror (errno) << std::endl;

    // Translate error codes indicating asynchronous connect
    // has been launched to a uniform EINPROGRESS.
    if (errno == EINTR) //Kostas: Irrelevant for rio; reconnect not implemented
        errno = EINPROGRESS;

    // Forward the error
    return -1;
}

void zmq::rio_connecter_t::close ()
{
    zmq_assert (s != retired_fd);
    socket->event_closed (endpoint, s);

    rioh_mailbox.consume();
    rioh_mailbox.close ();
    s = retired_fd;

    RapidIODestroyMailbox (mailbox);
}

#endif
