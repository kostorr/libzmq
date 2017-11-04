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


#ifndef __ZMQ_RIO_ENGINE_HPP_INCLUDED__
#define __ZMQ_RIO_ENGINE_HPP_INCLUDED__

#if defined ZMQ_HAVE_RIO

#include "io_object.hpp"
#include "i_engine.hpp"
#include "address.hpp"
#include "i_encoder.hpp"
#include "i_decoder.hpp"
#include "rio_address.hpp"
#include "msg.hpp"
#include "rioh_mailbox.hpp"

#define RIO_MSG_SIZE 0x1000
#define RIO_HEADER_SIZE 0x0014 
#define RIO_DMA_BUF_SIZE (TRIOSYS_DRV_DMA_RES_MEM_SIZE/RIO_DMA_BUF_NUM)
#define RIO_DMA_BUF_MULTI (!(TRIOSYS_DRV_DMA_RES_MEM_SIZE == RIO_DMA_BUF_SIZE))

namespace zmq
{
    class io_thread_t;
    class session_base_t;
    class msg_t;
    struct addresss_t;

    class rio_engine_t : public io_object_t, public i_engine
    {
        public:

            enum error_reason_t {
                protocol_error,
                connection_error,
                timeout_error
            };

            rio_engine_t (rio_conn_info rci_, const std::string &endpoint_);
            ~rio_engine_t ();

            //  i_engine interface implementation.
            //  Plug the engine to the session.
            void plug (zmq::io_thread_t *io_thread_, class session_base_t *session_);

            //  Unplug the engine from the session.
            void unplug ();

            //  Function to handle network disconnections.
            void error (error_reason_t reason);

            //  Terminate and deallocate the engine. Note that 'detached'
            //  events are not fired on termination.
            void terminate ();

            //  This method is called by the session to signalise that more
            //  messages can be written to the pipe.
            void restart_input ();

            //  This method is called by the session to signalise that there
            //  are messages to send available.
            void restart_output ();

            void zap_msg_available () {};

            void in_event ();
            void out_event ();

            void update_written_slots (int update_slot);
            void update_read_slots (int update_slot);

        private:
            std::string endpoint;

            // zmq Socket
            zmq::socket_base_t *zmq_socket;

            //  True if the engine doesn't have any message to encode.
            bool output_stopped;
            bool input_stopped;

            bool plugged;
            session_base_t* session;
            handle_t handle;

            unsigned char *inpos;
            size_t insize;
            i_decoder *decoder;

            unsigned char *outpos;
            size_t outsize;
            i_encoder *encoder;
            msg_t msg;
        
            // Address to send to. Owned by session_base_t.
            // rio_address_t addr;
            uint16_t remote_dest_id;
            uint16_t local_dest_id;

            //  Underlying "socket" (=fd)
            fd_t s;

            //  Heartbeat stuff
            enum {
                heartbeat_ivl_timer_id = 0x80,
                heartbeat_timeout_timer_id = 0x81,
                heartbeat_ttl_timer_id = 0x82
            };
            bool has_ttl_timer;
            bool has_timeout_timer;
            bool has_heartbeat_timer;
            int heartbeat_timeout;

            bool io_error;

            // RapidIO fields

            rioh_mailbox_t rioh_mailbox;
            //riomp_sock_t socket;

            riomp_mport_t *mport_hnd;
            uint64_t *dma_mem_hnd; //Handle to the dma memory needed by libs
            uint64_t tx_tgt_addr; //Base DMA target address of remote end
            void *rx_buf; //Mapped DMA address - local
            void *tx_buf; //Buffer used to send DMA memory

            int conn_id; //Connection identifier assigned to this engine by the server

            int in_local_pos; //Local position referring to current DMA cell being received
            int out_local_pos; //Local position referring to current DMA cell being sen_
            int written_slots[RIO_DMA_BUF_NUM/NUM_CONNS]; // flag array for dma buf positions that have been written
            int written_slot_index;
            int read_slots[RIO_DMA_BUF_NUM/NUM_CONNS]; // flag array for dma buf positions that have been read
    };
}

#endif
#endif
