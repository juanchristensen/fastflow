/* ***************************************************************************
 *
 *  FastFlow is free software; you can redistribute it and/or modify it
 *  under the terms of the GNU Lesser General Public License version 3 as
 *  published by the Free Software Foundation.
 *  Starting from version 3.0.1 FastFlow is dual licensed under the GNU LGPLv3
 *  or MIT License (https://github.com/ParaGroup/WindFlow/blob/vers3.x/LICENSE.MIT)
 *
 *  This program is distributed in the hope that it will be useful, but WITHOUT
 *  ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 *  FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public
 *  License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public License
 *  along with this program; if not, write to the Free Software Foundation,
 *  Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 ****************************************************************************
 */
/* Authors: 
 *   Nicolo' Tonci
 *   Massimo Torquati
 */

#ifndef FF_DRECEIVER_H
#define FF_DRECEIVER_H


#include <iostream>
#include <sstream>
#include <ff/ff.hpp>
#include <ff/distributed/ff_network.hpp>
#include <ff/distributed/ff_dgroups.hpp>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <arpa/inet.h>
#include <cereal/cereal.hpp>
#include <cereal/archives/portable_binary.hpp>
#include <cereal/types/vector.hpp>
#include <cereal/types/polymorphic.hpp>

using namespace ff;

class ff_dreceiver: public ff_monode_t<message_t> { 
protected:

    static int sendRoutingTable(const int sck, const std::vector<int>& dest){
        dataBuffer buff; std::ostream oss(&buff);
		cereal::PortableBinaryOutputArchive oarchive(oss);
		oarchive << dest;

        size_t sz = htobe64(buff.getLen());
        struct iovec iov[2];
        iov[0].iov_base = &sz;
        iov[0].iov_len = sizeof(sz);
        iov[1].iov_base = buff.getPtr();
        iov[1].iov_len = buff.getLen();

        if (writevn(sck, iov, 2) < 0){
            error("Error writing on socket the routing Table\n");
            return -1;
        }

        return 0;
    }

    virtual int handshakeHandler(const int sck){
        // ricevo l'handshake e mi salvo che tipo di connessione è
        size_t size;
        struct iovec iov; iov.iov_base = &size; iov.iov_len = sizeof(size);
        switch (readvn(sck, &iov, 1)) {
           case -1: error("Error reading from socket\n"); // fatal error
           case  0: return -1; // connection close
        }

        size = be64toh(size);

        char groupName[size];
        if (readn(sck, groupName, size) < 0){
            error("Error reading from socket groupName\n"); return -1;
        }
        std::vector<int> reachableDestinations;
        for(const auto& [key, value] : this->routingTable) reachableDestinations.push_back(key);

        return this->sendRoutingTable(sck, reachableDestinations);
    }

    virtual void registerEOS(int sck){
        neos++;
    }

    virtual void forward(message_t* task, int){
        if (task->chid == -1) ff_send_out(task);
        else ff_send_out_to(task, this->routingTable[task->chid]); // assume the routing table is consistent WARNING!!!
    }

    virtual int handleBatch(int sck){
        int requestSize;
        switch(readn(sck, reinterpret_cast<char*>(&requestSize), sizeof(requestSize))) {
		case -1: {			
			perror("readn");
			error("Something went wrong in receiving the number of tasks!\n");
			return -1;
		} break;
		case 0: return -1;
        }
		// always sending back the acknowledgement
        if (writen(sck, reinterpret_cast<char*>(&ACK), sizeof(ack_t)) < 0){
            if (errno != ECONNRESET && errno != EPIPE) {
                error("Error sending back ACK to the sender (errno=%d)\n",errno);
                return -1;
            }
        }
		
        requestSize = ntohl(requestSize);
        for(int i = 0; i < requestSize; i++)
            if (handleRequest(sck)<0) return -1;
        
        return 0;
    }

    virtual int handleRequest(int sck){
   		int sender;
		int chid;
        size_t sz;
        struct iovec iov[3];
        iov[0].iov_base = &sender;
        iov[0].iov_len = sizeof(sender);
        iov[1].iov_base = &chid;
        iov[1].iov_len = sizeof(chid);
        iov[2].iov_base = &sz;
        iov[2].iov_len = sizeof(sz);

        switch (readvn(sck, iov, 3)) {
		case -1: error("Error reading from socket errno=%d\n",errno); // fatal error
		case  0: return -1; // connection close
        }

        // convert values to host byte order
        sender = ntohl(sender);
        chid   = ntohl(chid);
        sz     = be64toh(sz);

        if (sz > 0){
            char* buff = new char [sz];
			assert(buff);
            if(readn(sck, buff, sz) < 0){
                error("Error reading from socket in handleRequest\n");
                delete [] buff;
                return -1;
            }
			message_t* out = new message_t(buff, sz, true);
			assert(out);
			out->sender = sender;
			out->chid   = chid;
            this->forward(out, sck);

            return 0;
        }

        registerEOS(sck);
        return -1;
    }

public:
    ff_dreceiver(ff_endpoint acceptAddr, size_t input_channels, std::map<int, int> routingTable = {std::make_pair(0,0)}, int coreid=-1)
		: input_channels(input_channels), acceptAddr(acceptAddr), routingTable(routingTable), coreid(coreid) {}

    int svc_init() {
  		if (coreid!=-1)
			ff_mapThreadToCpu(coreid);

        #ifdef LOCAL
            if ((listen_sck=socket(AF_LOCAL, SOCK_STREAM, 0)) < 0){
                error("Error creating the socket\n");
                return -1;
            }
            
            struct sockaddr_un serv_addr;
            memset(&serv_addr, '0', sizeof(serv_addr));
            serv_addr.sun_family = AF_LOCAL;
            strncpy(serv_addr.sun_path, acceptAddr.address.c_str(), acceptAddr.address.size()+1);
        #endif

        #ifdef REMOTE
            if ((listen_sck=socket(AF_INET, SOCK_STREAM, 0)) < 0){
                error("Error creating the socket\n");
                return -1;
            }

            int enable = 1;
            // enable the reuse of the address
            if (setsockopt(listen_sck, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(int)) < 0)
                error("setsockopt(SO_REUSEADDR) failed\n");

            struct sockaddr_in serv_addr;
            serv_addr.sin_family = AF_INET; 
            serv_addr.sin_addr.s_addr = INADDR_ANY; // still listening from any interface
            serv_addr.sin_port = htons( acceptAddr.port );

        #endif

        if (bind(listen_sck, (struct sockaddr*)&serv_addr,sizeof(serv_addr)) < 0){
            error("Error binding\n");
            return -1;
        }

        if (listen(listen_sck, MAXBACKLOG) < 0){
            error("Error listening\n");
            return -1;
        }
        
        return 0;
    }

    void svc_end() {
        close(this->listen_sck);		
#ifdef LOCAL
		unlink(this->acceptAddr.address.c_str());
#endif
    }
    /* 
        Here i should not care of input type nor input data since they come from a socket listener.
        Everything will be handled inside a while true in the body of this node where data is pulled from network
    */
    message_t *svc(message_t* task) {        
        fd_set set, tmpset;
        // intialize both sets (master, temp)
        FD_ZERO(&set);
        FD_ZERO(&tmpset);

        // add the listen socket to the master set
        FD_SET(this->listen_sck, &set);

        // hold the greater descriptor
        int fdmax = this->listen_sck; 

        while(neos < input_channels){
            // copy the master set to the temporary
            tmpset = set;

            switch(select(fdmax+1, &tmpset, NULL, NULL, NULL)){
                case -1: error("Error on selecting socket\n"); return EOS;
                case  0: continue;
            }

            for(int idx=0; idx <= fdmax; idx++){
	            if (FD_ISSET(idx, &tmpset)){
                    if (idx == this->listen_sck) {
                        int connfd = accept(this->listen_sck, (struct sockaddr*)NULL ,NULL);
                        if (connfd == -1){
                            error("Error accepting client\n");
                        } else {
                            FD_SET(connfd, &set);
                            if(connfd > fdmax) fdmax = connfd;

                            this->handshakeHandler(connfd);
                        }
                        continue;
                    }
                    
                    if (this->handleBatch(idx) < 0){
                        close(idx);
                        FD_CLR(idx, &set);

                        // update the maximum file descriptor
                        if (idx == fdmax)
                            for(int ii=(fdmax-1);ii>=0;--ii)
                                if (FD_ISSET(ii, &set)){
                                    fdmax = ii;
                                    break;
                                }
						
                    }
					
                }
            }
        }
		
        return this->EOS;
    }

protected:
    size_t neos = 0;
    size_t input_channels;
    int listen_sck;
    ff_endpoint acceptAddr;	
    std::map<int, int> routingTable;
	int coreid;
    ack_t ACK;
};


class ff_dreceiverH : public ff_dreceiver {

    std::vector<int> internalDestinations;
    std::map<int, bool> isInternalConnection;
    std::set<std::string> internalGroupsNames;
    size_t internalNEos = 0, externalNEos = 0;
    long next_rr_destination = 0;

    void registerEOS(int sck){
        neos++;
        size_t internalConn = std::count_if(std::begin(isInternalConnection),
                                            std::end  (isInternalConnection),
                                            [](std::pair<int, bool> const &p) {return p.second;});

        if (!isInternalConnection[sck]){
            if (++externalNEos == (isInternalConnection.size()-internalConn))
				for(size_t i = 0; i < this->get_num_outchannels()-1; i++) ff_send_out_to(this->EOS, i);
        } else
			if (++internalNEos == internalConn)
				ff_send_out_to(this->EOS, this->get_num_outchannels()-1);
        
        
    }

    virtual int handshakeHandler(const int sck){
        size_t size;
        struct iovec iov; iov.iov_base = &size; iov.iov_len = sizeof(size);
        switch (readvn(sck, &iov, 1)) {
           case -1: error("Error reading from socket\n"); // fatal error
           case  0: return -1; // connection close
        }

        size = be64toh(size);

        char groupName[size];
        if (readn(sck, groupName, size) < 0){
            error("Error reading from socket groupName\n"); return -1;
        }
        
        bool internalGroup = internalGroupsNames.contains(std::string(groupName,size));

        isInternalConnection[sck] = internalGroup; // save somewhere the fact that this sck represent an internal connection

        if (internalGroup) return this->sendRoutingTable(sck, internalDestinations);

        std::vector<int> reachableDestinations;
        for(const auto& [key, value] :  this->routingTable) reachableDestinations.push_back(key);
        return this->sendRoutingTable(sck, reachableDestinations);
    }

    void forward(message_t* task, int sck){
        if (isInternalConnection[sck]) ff_send_out_to(task, this->get_num_outchannels()-1);
        else if (task->chid != -1) ff_send_out_to(task, this->routingTable[task->chid]);
        else {
            ff_send_out_to(task, next_rr_destination);
            next_rr_destination = (next_rr_destination + 1) % (this->get_num_outchannels()-1);
        }
    }

public:
    ff_dreceiverH(ff_endpoint acceptAddr, size_t input_channels, std::map<int, int> routingTable = {{0,0}}, std::vector<int> internalRoutingTable = {0}, std::set<std::string> internalGroups = {}, int coreid=-1) 
    : ff_dreceiver(acceptAddr, input_channels, routingTable, coreid), internalDestinations(internalRoutingTable), internalGroupsNames(internalGroups) {

    }

};

#endif
