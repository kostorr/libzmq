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

/* Used for debugging */
#include <iostream>

#include <algorithm>

#include "precompiled.hpp"
#include "rioh_mailbox.hpp"
#include "err.hpp"
#include "rio_engine.hpp"


zmq::rioh_mailbox_t::rioh_mailbox_t (bool active_, std::string type_, class rio_engine_t *rio_engine_, uint32_t local_dest_id, uint32_t remote_dest_id)
{
    active = active_;

    mport_id = 0;

    // Initialize mport handle and related properties
    int ret = 0;
    ret = riomp_mgmt_mport_create_handle (mport_id, flags, &mport_hnd);

    if (ret) {
        std::cerr << "mport_create_handle failed " << strerror(errno) << std::endl;
    }
        
    if (!riomp_mgmt_query (mport_hnd, &prop)){
        //riomp_mgmt_display_info (&prop);

        if (prop.link_speed == 0){
            std::cerr << "SRIO link is down" << std::endl;
            riomp_mgmt_mport_destroy_handle (&mport_hnd);
        }
    } else
        std::cerr << "Failed to obtain mport information. Using default configuration" << std::endl;

    if (active) {
        if (!type_.compare("ENGINE")){
            uint32_t number_of_eps = -1;
            did_val_t *destids = NULL;
            ret = riomp_mgmt_get_ep_list(0, &destids, &number_of_eps);
            if (ret)
                std::cerr << "riomp_mgmt_get_ep_list failed" << std::endl;

            std::map <std::pair<int, int>, int> dbrange_map = RapidIODBRangeMap (destids, number_of_eps, local_dest_id);

            riomp_mgmt_free_ep_list(&destids);

            db_rioid = remote_dest_id;
            db_start_local = dbrange_map.at(std::make_pair(local_dest_id, remote_dest_id));
            db_end_local = db_start_local + RIO_DMA_BUF_NUM/NUM_CONNS - 1;
            db_start_remote = dbrange_map.at(std::make_pair(remote_dest_id, local_dest_id));
            db_end_remote = db_start_remote + RIO_DMA_BUF_NUM/NUM_CONNS - 1;

        } else if (!type_.compare("LISTENER"))
            db_start_local = db_end_local = RIO_CONNECT;
        else //error - use invalid range
            db_start_local = db_end_local = 0x5a5b; 

        riomp_mgmt_set_event_mask (mport_hnd, RIO_EVENT_DOORBELL);
        
        // Enable local doorbell range 
        //std::cout << "Type: " << type_ << ". Enabling local range [" << db_start_local << " - " << db_end_local << "]" << std::endl;
        ret = riomp_mgmt_dbrange_enable (mport_hnd, db_rioid, db_start_local, db_end_local);
        if (ret)
            std::cerr << "riomp_mgmt_dbrange_enable local failed: ret=" << ret;

        if (!type_.compare("ENGINE")){
            // Enable remote doorbell range

            //std::cout << "Type: " << type_ << ". Enabling remote range [" << db_start_remote << " - " << db_end_remote << "]" << std::endl;
            ret = riomp_mgmt_dbrange_enable (mport_hnd, db_rioid, db_start_remote, db_end_remote);
            if (ret)
                std::cerr << "riomp_mgmt_dbrange_enable remote failed: ret=" << ret; 
        }

        // Prepare the needed references for the thread
        dbe = new doorbell_essentials();
        dbe->mport = &mport_hnd;
        dbe->signaler = &signaler;
        dbe->isrunning = &isrunning;
        dbe->mailbox = this;
        dbe->db_start_local = &db_start_local;
        dbe->db_start_remote = &db_start_remote;

        if (!type_.compare("ENGINE"))
            dbe->engine = rio_engine_;

        doorbell_thread.start(loop, dbe);
    }
}

zmq::rioh_mailbox_t::~rioh_mailbox_t () {
    // Work around problem that other threads might still be in our
    // send() method, by waiting on the mutex before disappearing.
    //sync.lock ();
    //sync.unlock (); //Kostas: How did this come to be??

}

zmq::fd_t zmq::rioh_mailbox_t::get_fd () const
{
    return signaler.get_fd ();
}

void zmq::rioh_mailbox_t::loop (void *arg_)
{
    doorbell_essentials *dbe = (doorbell_essentials *) arg_;

    struct riomp_mgmt_event ev;
    rioh_mailbox_t * mailbox = dbe->mailbox;
    while(*(dbe->isrunning)) {

        int ret = riomp_mgmt_get_event(*(dbe->mport), &ev);
        if (ret < 0){
            if (ret == -EAGAIN)
                continue;
            else{
                std::cerr << "Failed to read event, err=" << ret << std::endl;
                break;
            }
        }

        if (ev.header == RIO_EVENT_DOORBELL){

            // I can start a thread for each event. But again, this isn't a watertight solution.

            //std::cout << "Got " << ev.u.doorbell.payload << " from " << ev.u.doorbell.did_val << std::endl;

            uint16_t opcode = ev.u.doorbell.payload;
            mailbox->set_signaling_destID (ev.u.doorbell.did_val);
            if (opcode == RIO_CONNECT)
                dbe->signaler->send ();
            else{
                if (opcode >= *(dbe->db_start_local) && (opcode < (*(dbe->db_start_local) + RIO_DMA_BUF_NUM/NUM_CONNS)) ){ //update position to read from
                    dbe->engine->update_written_slots (opcode - *(dbe->db_start_local)); //set remote pos relative to db_local_start
                    dbe->signaler->send ();
                }

                if (opcode >= *(dbe->db_start_remote) && (opcode < (*(dbe->db_start_remote) + RIO_DMA_BUF_NUM/NUM_CONNS)) ){ //update position to write to
                    dbe->engine->update_read_slots (opcode - *(dbe->db_start_remote)); //set flag_pos relative to db_remote_start
                }
            }
        }

    }

}

void zmq::rioh_mailbox_t::send_db (uint16_t destID, int opcode)
{
    /* Send a doorbell to the destID endpoint here holding the opcode data*/
    struct riomp_mgmt_event ev;

    //std::cout << "Sending doorbell " << opcode << " to " << destID << std::endl;

    ev.header = RIO_EVENT_DOORBELL;
    ev.u.doorbell.did_val = destID;
    ev.u.doorbell.payload = opcode;

    int ret = riomp_mgmt_send_event(mport_hnd, &ev);
    if (ret)
        std::cerr << "Write DB event failed " << ret << std::endl;
}

void zmq::rioh_mailbox_t::set_signaling_destID (uint32_t signaling_destID_)
{
    signaling_destID = signaling_destID_;
}

uint32_t zmq::rioh_mailbox_t::get_db_start_local ()
{
    return db_start_local;
}

uint32_t zmq::rioh_mailbox_t::get_db_start_remote ()
{
    return db_start_remote;
}

uint32_t zmq::rioh_mailbox_t::consume ()
{
    signaler.recv();
    return signaling_destID; //TODO: This could very well introduce a race condition
}

void zmq::rioh_mailbox_t::close ()
{
    // Disable doorbell range
    if (active) {
        isrunning = false;
        doorbell_thread.stop();
        delete dbe;

        int ret = riomp_mgmt_dbrange_disable(mport_hnd, db_rioid, db_start_local, db_end_local);
        if (ret) {
            std::cerr << "riomp_mgmt_dbrange_disable local failed" << std::endl;
            return;
        }

        if (db_start_remote != 0 && db_end_remote != 0){
            ret = riomp_mgmt_dbrange_disable(mport_hnd, db_rioid, db_start_remote, db_end_remote);
            if (ret) {
                std::cerr << "riomp_mgmt_dbrange_disable remote failed" << std::endl;
                return;
            }
        }

        ret = riomp_mgmt_mport_destroy_handle(&mport_hnd);
        if (ret) {
            std::cerr << "riomp_mgmt_mport_destroy_handle failed" << std::endl;
            return;
        }

    }
}
