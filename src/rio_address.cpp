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

#include "rio_address.hpp"

#if defined ZMQ_HAVE_RIO

#include <climits>
#include <string>
#include <sstream>
#include <iostream>
#include "err.hpp"

zmq::rio_address_t::rio_address_t ()
{
        dest_id = channel = 0;
}

zmq::rio_address_t::~rio_address_t ()
{
}

int zmq::rio_address_t::resolve (const char *name_)
{
    const char *delimiter = strchr (name_, ':');

    std::string dest_id_str (name_, delimiter - name_);
    std::string channel_str (delimiter + 1);
    dest_id = stoul (dest_id_str);
    channel = stoul (channel_str);

    return 0;
}

int zmq::rio_address_t::to_string (std::string &addr_)
{
    std::stringstream s;
    s << "rio://";
    s << dest_id << ':' << channel;

    addr_ = s.str ();
    return 0;
}

uint16_t zmq::rio_address_t::get_dest_id ()
{
    return dest_id;
}

uint16_t zmq::rio_address_t::get_channel ()
{
    return channel;
}

#endif 
