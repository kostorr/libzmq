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

#include "rio_listener.hpp"

#if defined ZMQ_HAVE_RIO

#include <iostream>
#include <sys/eventfd.h>
#include <zmq.h>
#include "io_thread.hpp"
#include "session_base.hpp"
#include "ip.hpp"
#include "rio_engine.hpp"

zmq::rio_listener_t::rio_listener_t (io_thread_t *io_thread_,
        socket_base_t *socket_, const options_t &options_):
    own_t (io_thread_, options_),
    io_object_t (io_thread_),
    s (retired_fd),
    socket (socket_),
    conn_id (0),
    rioh_mailbox (true, (std::string) "LISTENER", NULL)
{
}

zmq::rio_listener_t::~rio_listener_t ()
{
    zmq_assert (s == retired_fd);
}

void zmq::rio_listener_t::process_plug ()
{
    // Register the rioh_mailbox's fd with the poller
    handle = add_fd (s);
    set_pollin (handle);
}

void zmq::rio_listener_t::process_term (int linger_)
{
    rm_fd (handle);
    close ();
    own_t::process_term (linger_);
}

void zmq::rio_listener_t::in_event ()
{
    int dest_id = rioh_mailbox.consume (); //Consume in_event so it doesn't trigger us again
    riomp_sock_t sock = accept();

    if (sock == NULL) {
        socket->event_accept_failed (endpoint, zmq_errno());
        return;
    }

    // Initialize DMA
    mport_hnd = (riomp_mport_t *) malloc (sizeof(riomp_mport_t)); //Allocate mport_hnd before calling DMAPrep
    dma_mem_hnd = (uint64_t *) malloc (sizeof(uint64_t)); // Same for dma_mem_hnd
    int rc = RapidIODMAPrep(mport_hnd, &rx_tgt_addr, dma_mem_hnd, &rx_buf);
    if (rc){
        std::cerr << "RapidIODMAPrep failed";
        close();
        return;
    }

    // Exchange relevant information with remote end
    rc = RapidIODMAHandshakeListener(sock, rx_tgt_addr, &tx_tgt_addr, conn_id, msg_tx_buf, msg_rx_buf);
    if (rc){
        std::cerr << "RapidIODMAHandshake failed";
        close();
        return;
    }
    /*std::cout << " Handshaking done" << std::endl;
    printf("tx_tgt_addr is 0x%lx\n", tx_tgt_addr);
    printf("mport_hnd is 0x%lx\n", mport_hnd)
    printf("conn_id is %d\n", conn_id);*/

    rio_conn_info rci = {};
    rci.mport_hnd = mport_hnd;
    rci.dma_mem_hnd = dma_mem_hnd;
    rci.remote_dest_id = dest_id; //pass the remote destination id
    rci.local_dest_id = local_dest_id; //pass the local destination id
    rci.tx_tgt_addr = tx_tgt_addr;
    rci.rx_buf = rx_buf;
    rci.conn_id = conn_id;

    // Create the engine object for this connection
    rio_engine_t *engine = new (std::nothrow)
        rio_engine_t (rci, endpoint);
    alloc_assert(engine);

    // Update connection id after it's passed down to the engine
    conn_id++;

    // Choose I/O thread to run connecter in. Given that we are already
    // running in an I/O thread, there must be at least one available.
    io_thread_t *io_thread = choose_io_thread (options.affinity);
    zmq_assert (io_thread);

    // Create and launch a sesion object.
    session_base_t *session = session_base_t::create (io_thread, false, socket,
            options, NULL);
    errno_assert (session);
    session->inc_seqnum ();
    launch_child (session);
    send_attach (session, engine, false);

    socket->event_accepted (endpoint, s);
}

int zmq::rio_listener_t::get_address (std::string &addr_)
{
    return my_addr.to_string(addr_);
}

int zmq::rio_listener_t::set_address (const char *addr_)
{
    // Create addr on stack for auto-cleanup
    std::string addr (addr_);

    // Initialise the address structure.
    rio_address_t address;
    int rc = address.resolve (addr.c_str());
    if (rc)
        return -1;

    address.to_string (endpoint);

    mport_id = 0;
    mailbox = (riomp_mailbox_t *)malloc(sizeof(riomp_mailbox_t));
    local_dest_id = address.get_dest_id();
    channel = address.get_channel();

    // Create a mailbox control structure
    rc = riomp_sock_mbox_create_handle (mport_id, 0, mailbox);
    if (rc)
        return -1;
   
    rio_socket = (riomp_sock_t *)malloc(sizeof(riomp_sock_t));

    // Create an unbound socket structure
    rc = riomp_sock_socket (*mailbox, rio_socket);
    if (rc)
        return -1;

    // Bind the listen channel to the opened mport dev
    rc = riomp_sock_bind (*rio_socket, channel);
    if (rc)
        return -1;

    // Listen for incoming connections
    rc = riomp_sock_listen (*rio_socket);
    if (rc)
        goto error;

    // Register the new file descriptor
    s = rioh_mailbox.get_fd ();

    socket->event_listening (endpoint , s);
    my_addr = address;
    return 0;

error:
    int err = errno;
    close ();
    errno = err;
    return -1;
}

void zmq::rio_listener_t::close ()
{
    //zmq_assert (s != retired_fd); //TODO: Handle this
    socket->event_closed (endpoint, s);
    s = retired_fd;
    rioh_mailbox.close ();

    RapidIODestroyMailbox (mailbox);
}

riomp_sock_t zmq::rio_listener_t::accept()
{
    zmq_assert (s != retired_fd);
    riomp_sock_t sock;

    // Create a new socket for accept
    int rc = riomp_sock_socket(*mailbox, &sock);
    if (rc){
        std::cerr << "riomp_sock_socket failed " << strerror (errno) << std::endl;
        return NULL;
    }

    // Accept one connection
    rc = riomp_sock_accept(*rio_socket, &sock, 3*60000, NULL);
    if (rc) {
        std::cerr << "rio_accept failed " << strerror(errno) << std::endl;
        return NULL;
    }

    return sock; //Return the riomp_sock for the *new* connection
}

#endif
