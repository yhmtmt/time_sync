// Copyright(c) 2020 Yohei Matsumoto, Tokyo University of Marine
// Science and Technology, All right reserved. 

// f_time_sync.hpp is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

// f_time_sync.hpp is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.

// You should have received a copy of the GNU General Public License
// along with f_time_sync.hpp.  If not, see <http://www.gnu.org/licenses/>. 

#ifndef F_TIME_SYNC_HPP
#define F_TIME_SYNC_HPP
#include "filter_base.hpp"
#include "ch_state.hpp"

//#define DEBUG_F_TIME_SYNC

// Filter for time synchronization.
// This uses simple time exchanging scheme using UDP. 
// The filter works both as server and client. 
// The filter works as server if the server address is not specified.
// The filter has five state.
// TRN : Transmitting a packet to the server with client's time stamp.
//       Then, move to WAI. 
// RCV : Wait a packet from client with time stamp. If recieved a packet,
//       the server's time is recorded and state is set to REP. 
//       If not, move to TRN or RCV. 
//       
// WAI : Wait the packet reply for TRN. If succeeded, move to FIX.
//
// REP : Replying server's timestamp to the client.
// 
// FIX : Calculating time offset and correct it. Then move to RCV.
//
// SLP : Sleep until next time to synchronize
//
// Server takes RCV and REP states, and client takes TRN, WAI, FIX and SLP states.
// Basic synchronization process is as follow
//         Server                          Client
// RCV (Waiting for packet)        TRN (Transmitting packet)
// RCV (Packet Received)           WAI (Waiting reply)
// REP (Reply)                     WAI (Reply received)
// RCV (Waiting for packet)        FIX (Determine time difference)
// RCV (Waiting for packet)        SLP (Waiting for next transmission)
//
// 
// The server basically wait packet from client in RCV state.
// The client pass the packet with client timestamp,and the server reply the
// packet with server timestamp. If the packet exchange succeeded without any
// disturbance, the time delta is calculated on the client.
// client  can use the value to correct the time of data retrieved from server.


class f_time_sync: public f_base
{
protected:

  ch_time_sync * m_ch_time_sync;
  
  enum e_mode{
    TRN, RCV, WAI, REP, FIX, SLP
  } mode;

  // The packet data structure and the object to be exchanged
  // between server and client
  struct s_tpkt{
    unsigned int id;
    long long tc1, ts1, ts2, tc2, del;
    
    long long calc_delta(){
      // Assume client time delay to, following equations are established
      //    ts1 - tc1 = d + R(Ts) + to
      //    tc2 - ts2 = d + R(Tc) - to
      // where d is the communication delay, R(Ts) and R(Tc) are the
      // cycle time dependent delay components of server and client. 
      // Ts and Tc is the cycle times. R(T) can be the value between
      // 0 and T. Then, 
      //    [(ts1 - tc1) - (tc2 - ts2)]/2 = to 
      // We add -to to current time.

      return ((ts1 - tc1) - (tc2 - ts2)) / 2;  
    };
    void pack(char * buf);
    void unpack(const char * buf);
  } m_trpkt;

  char m_trbuf[sizeof(s_tpkt)];
  bool m_verb;
  char m_host_dst[1024];       // Server's ip address (in client mode)
  unsigned short m_port_dst;   // Server's port (in client mode)
  unsigned short m_port;       // Server's port (in server mode)

  c_log log;
  bool replay;
  
  SOCKET m_sock;
  socklen_t m_sz_rep;          // Size of client address object
  sockaddr_in m_sock_addr_snd;
  sockaddr_in m_sock_addr_rep; // Client address 
  sockaddr_in m_sock_addr_rcv;
  int m_adjust_intvl;
  long long m_tnext_adj;
  int m_rcv_wait_count;
  int m_max_rcv_wait_count;
  bool sttrn();
  bool strcv();
  bool stwai();
  bool strep();
  bool stfix();
  bool stslp();
  bool clearpkts();
public:
  f_time_sync(const char * name);

  virtual ~f_time_sync()
  {
  }
	
  virtual bool init_run();
  virtual void destroy_run();
  virtual bool proc();
};

#endif
