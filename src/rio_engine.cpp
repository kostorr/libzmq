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

#include "platform.hpp"

#if defined ZMQ_HAVE_RIO

#include <sys/types.h>
#include <unistd.h>
#include <iostream>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "rio_engine.hpp"
#include "session_base.hpp"
#include "v2_protocol.hpp"
#include "err.hpp"
#include "ip.hpp"
#include "i_encoder.hpp"
#include "i_decoder.hpp"
#include "v2_encoder.hpp"
#include "v1_encoder.hpp"
#include "v2_decoder.hpp"
#include "v1_decoder.hpp"
#include "v2_protocol.hpp"
#include "raw_decoder.hpp"
#include "raw_encoder.hpp"
#include "likely.hpp"

#include <sys/time.h>

#include <mutex>

std::mutex mf,mr;

static struct timespec timediff(struct timespec start, struct timespec end)
{
	struct timespec temp;
	if ((end.tv_nsec - start.tv_nsec) < 0) {
		temp.tv_sec = end.tv_sec - start.tv_sec - 1;
		temp.tv_nsec = 1000000000 + end.tv_nsec - start.tv_nsec;
	} else {
		temp.tv_sec = end.tv_sec - start.tv_sec;
		temp.tv_nsec = end.tv_nsec - start.tv_nsec;
	}
	return temp;
}

zmq::rio_engine_t::rio_engine_t(rio_conn_info rci_, const std::string &endpoint_) :
    zmq_socket(NULL),
    endpoint (endpoint_),
    output_stopped (false),
    input_stopped (false),
    plugged (false),
    session(NULL),
    io_error(false),
    s (NULL),
    handle (NULL),
    inpos (NULL),
    insize (0),
    decoder (NULL),
    outpos (NULL),
    outsize (0),
    encoder (NULL),
    rioh_mailbox (true, "ENGINE", this, rci_.local_dest_id, rci_.remote_dest_id),
    in_local_pos (0),
    out_local_pos (0),
    written_slot_index (0)
{
    int rc = msg.init ();
    errno_assert (rc == 0);
    msg.init_size (RIO_DMA_BUF_SIZE);

    // Get connection info values
    // Should I move these to init list?
    mport_hnd = rci_.mport_hnd; 
    dma_mem_hnd = rci_.dma_mem_hnd;
    remote_dest_id = rci_.remote_dest_id;
    local_dest_id = rci_.local_dest_id;
    tx_tgt_addr = rci_.tx_tgt_addr;
    rx_buf = rci_.rx_buf;
    conn_id = rci_.conn_id;

    for (int i=0; i<RIO_DMA_BUF_NUM/NUM_CONNS; i++){
        read_slots[i] = 1; //assume everything is read on first iteration!
        written_slots[i] = 0; //and nothing is written :)
    }

    tx_buf = (char *) calloc (RIO_DMA_BUF_SIZE, sizeof(char));

    std::cout << "RIO_DMA_BUF_SIZE -> " << RIO_DMA_BUF_SIZE << std::endl;
}

zmq::rio_engine_t::~rio_engine_t()
{
    zmq_assert (!plugged);

    int rc = msg.close ();
    errno_assert (rc == 0);
}

void zmq::rio_engine_t::plug (io_thread_t* io_thread_, session_base_t *session_)
{
    zmq_assert (!plugged);
    plugged = true;

    zmq_assert (!session);
    zmq_assert (session_);
    session = session_;

    s = rioh_mailbox.get_fd ();

    //  Connect to I/O threads poller object.
    io_object_t::plug (io_thread_);
    handle = add_fd (s);
                                                
    set_pollout (handle);
    set_pollin (handle);

    /* encoder and decoder for msg <=> rio buffer */
    encoder = new (std::nothrow) v2_encoder_t (0);
    alloc_assert (encoder);
    
    decoder = new (std::nothrow) v2_decoder_t (RIO_DMA_BUF_SIZE, INT64_MAX);
    alloc_assert (decoder);

}

void zmq::rio_engine_t::terminate()
{
    unplug ();
    delete this;
}

void zmq::rio_engine_t::out_event()
{

    //std::cout << "In out_event with out_local_pos = " << out_local_pos << " and outsize = " << outsize << std::endl;
    // Pull the message from the session, possibly in pieces (in while)
    if (!outsize) {
        outpos = static_cast<unsigned char *> (tx_buf);
        outsize = encoder->encode (&outpos, (size_t) (RIO_DMA_BUF_SIZE));
        //std::cout << "Encode returned outsize: " << outsize << std::endl;
        while (outsize < (RIO_DMA_BUF_SIZE * sizeof(char))) {
            int rc = session->pull_msg (&msg);
            if (rc == -1){
                break;
            }
            encoder->load_msg (&msg);
            unsigned char *bufptr = outpos + outsize;
            size_t n = encoder->encode (&bufptr, (size_t) (RIO_DMA_BUF_SIZE - outsize)); // A: Only whatever is possible will be
                                                                                         // encoded here!
            zmq_assert (n > 0);
            if (outpos == NULL)
                outpos = bufptr;
            outsize += n;
        }

        if (outsize == 0) {
            output_stopped = true;
            reset_pollout (handle);
            return;
        }
    }

    int rc = 0;
    int length = outsize;

    uint64_t temp_tx_tgt_addr = 0;

    temp_tx_tgt_addr = tx_tgt_addr + (TRIOSYS_DRV_DMA_RES_MEM_SIZE/NUM_CONNS)*conn_id  + ((RIO_DMA_BUF_SIZE)*out_local_pos);

    // Poll, until the position we want to write to is available for writing
    while (true){
        mf.lock(); 
        if(read_slots[out_local_pos]){ //means data is now read!
            read_slots[out_local_pos] = 0; //set as not read for next iteration
            mf.unlock();
            break;
        }
        mf.unlock();
    }

    struct timespec wr_starttime, wr_endtime, time;
    float totaltime;

    rc = riomp_dma_write(*mport_hnd, remote_dest_id, 
            temp_tx_tgt_addr, tx_buf,
            length, RIO_DIRECTIO_TYPE_NWRITE_R,
            RIO_DIRECTIO_TRANSFER_SYNC,
            NULL);

    if(rc){
        std::cerr << "riomp_dma_write returned with " << rc << std::endl;
        error(connection_error);
        return;
    }

    rioh_mailbox.send_db (remote_dest_id, rioh_mailbox.get_db_start_remote() + out_local_pos); //==db_start_remote + local_pos
    if (RIO_DMA_BUF_MULTI){
        out_local_pos = (out_local_pos+1)%(RIO_DMA_BUF_NUM/(NUM_CONNS));
    }

    int nbytes = outsize; //we would like the nbytes here to be returned from our send call...
    outpos += nbytes;
    outsize -= nbytes;

    return;
}

void zmq::rio_engine_t::restart_output()
{
    if (likely (output_stopped)) {
        set_pollout (handle);
        output_stopped = false;
    }
    out_event ();
}

void zmq::rio_engine_t::in_event()
{

    rioh_mailbox.consume ();

    if (input_stopped) { //from stream_engine
        rm_fd (handle);
        io_error = true;
        return;
    }

    zmq_assert (decoder);

    // Poll, until the position we want to read from is written to
    if (!insize) {
        while (true){

            mr.lock(); 
            if(written_slots[in_local_pos]){ //means data is now written!
                written_slots[in_local_pos] = 0; //set as not written for next iteration
                mr.unlock();
                break;
            }
            mr.unlock();
        }


        size_t bufsize = 0;
        decoder->get_buffer (&inpos, &bufsize);

        char *temp_rx_buf = ((char *) rx_buf ) + (TRIOSYS_DRV_DMA_RES_MEM_SIZE/NUM_CONNS)*conn_id + (RIO_DMA_BUF_SIZE)*in_local_pos;
        
        // Point ZMQ memory to RIO memory
        inpos = (unsigned char*) temp_rx_buf;
        insize = (size_t) RIO_DMA_BUF_SIZE;
        
        decoder->resize_buffer(insize);
    }


    int rc = 0;
    size_t processed = 0;
    bool flag = false; //flag for terminating the decode process

    // Push message from decode to session
    while (insize > 0) {
        rc = decoder->decode (inpos, insize, processed);

        zmq_assert (processed <= insize);
        inpos += processed;
        insize -= RIO_DMA_BUF_SIZE;

        if (rc == 0 || rc == -1) //Nothing to push || error
            break;
        if (rc == 1){ // The whole message has been decoded and we thus need to break!
            flag = true;
        }
        rc = session->push_msg (decoder->msg ());
        if ((rc == -1) || (flag))
            break;
    }

    // Send remote end update!
    rioh_mailbox.send_db (remote_dest_id, rioh_mailbox.get_db_start_local() + in_local_pos); //== db_start_local

    if (RIO_DMA_BUF_MULTI){
	    in_local_pos = (in_local_pos+1)%(RIO_DMA_BUF_NUM/(NUM_CONNS));
    }

    if (rc == -1) {
        if (errno != EAGAIN) {
            error(protocol_error);
            session->flush ();
            return;
        }
        input_stopped = true;
        reset_pollin(handle);
    }

    session->flush ();
}

void zmq::rio_engine_t::error (error_reason_t reason)
{
    /*if (options.raw_socket && options.raw_notify) {
        //  For raw sockets, send a final 0-length message to the application
        //  so that it knows the peer has been disconnected.
        msg_t terminator;
        terminator.init();
        (this->*process_msg) (&terminator);
        terminator.close();
    }*/
    zmq_assert (session);
    //zmq_socket->event_disconnected (endpoint, s); // Kostas: Do I need this?
    session->flush ();
    session->rio_engine_error (reason);
    unplug ();
    delete this;
}

void zmq::rio_engine_t::unplug ()
{
    zmq_assert (plugged);
    plugged = false;

    //  Cancel all timers.
    /*if (has_handshake_timer) {
        cancel_timer (handshake_timer_id);
        has_handshake_timer = false;
    }

    if (has_ttl_timer) {
        cancel_timer (heartbeat_ttl_timer_id);
        has_ttl_timer = false;
    }

    if (has_timeout_timer) {
        cancel_timer (heartbeat_timeout_timer_id);
        has_timeout_timer = false;
    }

    if (has_heartbeat_timer) {
        cancel_timer (heartbeat_ivl_timer_id);
        has_heartbeat_timer = false;
    }*/ //poller was yammering on multiple clients, on shutdown

    //  Cancel all fd subscriptions.
    if (!io_error)
        rm_fd (handle);

    // Close the file descriptor of the mailbox
    rioh_mailbox.close ();

    //  Disconnect from I/O threads poller object.
    io_object_t::unplug ();

    // Close any RapidIO related stuff
    
    RapidIOReleaseDMAMemory(mport_hnd, dma_mem_hnd, tx_buf);

    session = NULL;
}


void zmq::rio_engine_t::restart_input()
{
    zmq_assert (input_stopped);
    zmq_assert (session != NULL);
    zmq_assert (decoder != NULL);

    int rc = session->push_msg (decoder->msg ());
    if (rc == -1) {
        if (errno == EAGAIN)
            session->flush ();
        else{
            std::cerr << "session->push_msg error, rc was" << rc << std::endl;
            error (protocol_error);
        }
        return;
    }

    bool flag = false; //flag for terminating the decode process

    while (insize > 0) {
        size_t processed = 0;
        rc = decoder->decode (inpos, insize, processed);

        zmq_assert (processed <= insize);
        inpos += processed;
        insize -= RIO_DMA_BUF_SIZE;

        if (rc == 0 || rc == -1)
            break;
        if (rc == 1){
            flag = true;
        }
        rc = session->push_msg (decoder->msg ());
        if ((rc == -1) || flag) 
            break;
    }

    if (rc == -1 && errno == EAGAIN)
        session->flush ();
    else
        if (io_error)
            error(connection_error);
        else
            if (rc == -1)
                error(protocol_error);
            else {
                input_stopped = false;
                set_pollin (handle);
                session->flush ();

                // Speculative read
                in_event ();
            }
}

void zmq::rio_engine_t::update_written_slots (int update_slot)
{
    mr.lock();
    written_slots[update_slot] = 1; // 1 means data is there, 0 means it's not
    mr.unlock();
}

void zmq::rio_engine_t::update_read_slots (int update_slot)
{
    mf.lock();
    read_slots[update_slot] = 1; // 1 means read, 0 means not read
    mf.unlock();
}

#endif
