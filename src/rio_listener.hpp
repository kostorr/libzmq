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

#ifndef __ZMQ_RIO_LISTENER_HPP_INCLUDED__
#define __ZMQ_RIO_LISTENER_HPP_INCLUDED__

#include "platform.hpp"

#if defined ZMQ_HAVE_RIO

#include <string>

#include "fd.hpp"
#include "own.hpp"
#include "stdint.hpp"
#include "io_object.hpp"


#include "rio.hpp"
#include "rio_address.hpp"
#include "rioh_mailbox.hpp"

namespace zmq
{

    class io_thread_t;
    class socket_base_t;

    class rio_listener_t : public own_t, public io_object_t
    {
    public:

        rio_listener_t (zmq::io_thread_t *io_thread_,
                zmq::socket_base_t *socket_, const options_t &options_);
        ~rio_listener_t ();

        //  Set address to listen on.
        int set_address (const char *addr_);

        // Get the bound address for use with wildcards
        int get_address (std::string &addr_);

    private:

        //  Handlers for incoming commands.
        void process_plug ();
        void process_term (int linger_);

        //  Handlers for I/O events.
        void in_event ();

        //  Close the listening socket.
        void close ();

        //  Accept the new connection. Returns the riomp_sock_t(socket) of the
        //  newly created connection. The function may return ?? 
        //  if the connection was dropped while waiting in the listen backlog.

        riomp_sock_t accept ();

        //  Underlying socket.
        fd_t s;

        // mport ID
        uint32_t mport_id;

        // local destination ID
        uint32_t local_dest_id;

        // channel
        uint16_t channel;

        // Used from get_address()
        rio_address_t my_addr;

        //  Handle corresponding to the listening socket.
        handle_t handle;

        //  Socket the listerner belongs to.
        zmq::socket_base_t *socket;

        // String representation of endpoint to bind to
        std::string endpoint;

        // RapidIO fields
        riomp_sock_t *rio_socket;

        riomp_mailbox_t *mailbox;

        riomp_mport_t *mport_hnd;

        rioh_mailbox_t rioh_mailbox;
        
        uint64_t *dma_mem_hnd; //Handle needed by library functions

        uint64_t rx_tgt_addr; //Base DMA target address - local
        uint64_t tx_tgt_addr; //Base DMA target address - remote

        void *rx_buf; //Pointer to mapped - local - DMA memory
        
        int conn_id; //Connection identifier to handle multiple clients

        rapidio_mport_socket_msg *msg_tx_buf;
        rapidio_mport_socket_msg *msg_rx_buf;

        rio_listener_t (const rio_listener_t&);
        const rio_listener_t &operator = (const rio_listener_t&);
    };

}

#endif

#endif

