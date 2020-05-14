// Copyright(c) 2020 Yohei Matsumoto, Tokyo University of Marine
// Science and Technology, All right reserved. 

// f_time_sync.cpp is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

// f_time_sync.cpp is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.

// You should have received a copy of the GNU General Public License
// along with f_time_sync.cpp.  If not, see <http://www.gnu.org/licenses/>. 


#include "f_time_sync.hpp"
DEFINE_FILTER(f_time_sync)

void f_time_sync::s_tpkt::pack(char * buf)
{
  *((unsigned int*)buf) = id;
  buf += sizeof(unsigned int);
  *((long long*)buf) = tc1;
  buf += sizeof(long long);
  *((long long*)buf) = ts1;
  buf += sizeof(long long);
  *((long long*)buf) = tc2;
  buf += sizeof(long long);
  *((long long*)buf) = ts2;
  buf += sizeof(long long);
  *((long long*)buf) = del;
}

void f_time_sync::s_tpkt::unpack(const char * buf)
{
  id = *((unsigned int*)buf);
  buf += sizeof(unsigned int);
  tc1 = *((long long*)buf);
  buf += sizeof(long long);
  ts1 = *((long long*)buf);
  buf += sizeof(long long);
  tc2 = *((long long*)buf);
  buf += sizeof(long long);
  ts2 = *((long long*)buf);
  buf += sizeof(long long);
  del = *((long long*)buf);
}

f_time_sync::f_time_sync(const char * name): f_base(name),
					     m_ch_time_sync(nullptr),
					     m_verb(false),
					     mode(RCV), m_adjust_intvl(10),
					     m_tnext_adj(0),
					     m_max_rcv_wait_count(1000),
					     replay(false)
{
  m_host_dst[0] = '\0';
  register_fpar("verb", &m_verb, "Verbose for debug.");
  register_fpar("port", &m_port, "UDP port.");
  register_fpar("port_svr", &m_port_dst, "Server UDP port.");
  register_fpar("host_svr", m_host_dst, 1024, "Server address.");
  register_fpar("Tadj", &m_adjust_intvl,
		"Time interval adjustment occurs in second.");
  register_fpar("MaxWaitCount", &m_max_rcv_wait_count,
		"Wait count for recieving reply packet.");
  register_fpar("ch_time_sync", (ch_base**)&m_ch_time_sync,
		typeid(ch_time_sync).name(), "Time synchronization channel");
  register_fpar("replay", &replay, "Replay flag");
}

bool f_time_sync::init_run()
{  
  if(replay){
    if(!m_ch_time_sync){
      spdlog::error("[{}] Replay mode requires channel connection.");
      return false;
    }
    if(!log.init(f_base::get_data_path(), get_name(), replay)){
      spdlog::error("[{}] Failed to open log file.", get_name());
      return false;
    }
    return true;
  }else{
    if(m_host_dst[0] != '\0' && m_ch_time_sync){
      if(!log.init(f_base::get_data_path(), get_name(), replay)){
	spdlog::error("[{}] Failed to open log file.", get_name());
	return false;
      }
    }
  }
  
  
  m_sock = socket(AF_INET, SOCK_DGRAM, 0);	
  if(set_sock_nb(m_sock) != 0)
    return false;

  m_sock_addr_snd.sin_family =AF_INET;
  m_sock_addr_snd.sin_port = htons(m_port_dst);
  set_sockaddr_addr(m_sock_addr_snd, m_host_dst);

  m_sock_addr_rep.sin_family = AF_INET;
  m_sock_addr_snd.sin_port = htons(m_port_dst);
  set_sockaddr_addr(m_sock_addr_rep, m_host_dst);

  m_sock_addr_rcv.sin_family =AF_INET;
  m_sock_addr_rcv.sin_port = htons(m_port);
  set_sockaddr_addr(m_sock_addr_rcv);
  if(::bind(m_sock, (sockaddr*)&m_sock_addr_rcv,
	    sizeof(m_sock_addr_rcv)) == SOCKET_ERROR){
    cerr << "Socket error " << strerror(errno) << endl;
    return false;
  }

  if(m_host_dst[0] == '\0')
    mode = RCV; // Server mode
  else
    mode = TRN; // Client mode
  
  return true;
}

void f_time_sync::destroy_run()
{
  closesocket(m_sock);
  m_sock = -1;
}

bool f_time_sync::proc()
{
  if(replay && m_ch_time_sync != nullptr){
    long long t, delta;
    size_t sz;
    if(!log.read(t, (unsigned char*)&delta, sz)){
      spdlog::error("[{}] Failed to get delta.", get_name());
      return false;
    }
    m_ch_time_sync->set_time_delta(t, delta);    
    return true;
  }
  
  // client
  // TRN->WAI->
  //    ->FIX->SLP->TRN
  
  // server
  // RCV->REP->RCV
  
  switch(mode){
  case TRN: // -> RCV or WAI
    if(!sttrn())
      return false;
    break;
  case WAI: // -> RCV or TRN or FIX
    if(!stwai())
      return false;
    break;
  case RCV: // -> RCV or TRN
    if(!strcv())
      return false;
    break;
  case REP: // -> RCV
    if(!strep())
      return false;
    break;
  case FIX: // -> RCV
    if(!stfix())
      return false;
    break;
  case SLP:
    if(!stslp())
      return false;
    break;
  }

  return true;
}

// State TRN transmits time synchronization request packet.
// If the filter is not configured as client, means that destination
// host address is not specified, the request packet never transmitted, and 
// the state never move to WAI.
// State Transition
// TRN -> WAI if destination address is defined,
//            after sending initial time synchronization request 
// TRN -> RCV if destination address is not defined.
//            In this case, the state never be back to TRN.
bool f_time_sync::sttrn()
{
    
  // transmmit packet to the master server
  memset((void*) &m_trpkt, 0, sizeof(s_tpkt));
  m_trpkt.id = ((unsigned int) rand() << 16) | (unsigned int) rand();
  m_trpkt.tc1 = get_time();
#ifdef DEBUG_F_TIME_SYNC
  cout << "Sending tsync request with id:" << m_trpkt.id
       << " Tc1:" << m_trpkt.tc1 << endl;
#endif
  socklen_t sz = sizeof(m_sock_addr_snd);
  m_trpkt.pack(m_trbuf);
  sendto(m_sock, (const char*)m_trbuf, sizeof(s_tpkt),
	 0, (sockaddr*)&m_sock_addr_snd, sz);
  m_rcv_wait_count = 0;
  mode = WAI;
  
  return true;
}

// WAI waits a time synchronization packet as a reply to the packet previously
// sent in TRN state.
// The successful reception of the reply packet moves the state to FIX.
// If recieving the reply packet is distarbed by other time synchronization
// request, the time synchronization process is aborted, and again the state
// move to TRN. Simultaneously, for the time synchronization request,
// "del" command is sent to the client. "del" command include time the client
// should wait for sending synchronization request again.
// If "del" command is recieved, the state is forced to be RCV,
// until the wait time goes by. 
// 
// STATE Transition
// WAI->TRN if other time synchronization request is recieved faster.
// WAI->RCV if time synchronization request is denied.
// WAI->FIX if the reply packet is recieved first.
bool f_time_sync::stwai()

{
  s_tpkt rcvpkt;
  socklen_t sz = sizeof(m_sock_addr_rep);
  long long del = m_adjust_intvl * SEC;
  int count = 0; // error counter

  // In the loop all the packets recieved at this moment are consumed.
  while(1){
    timeval to;
    to.tv_sec = 0;
    to.tv_usec = 10000; // waits 10msec
    fd_set fdrd, fder;
    FD_ZERO(&fdrd);
    FD_ZERO(&fder);
    FD_SET(m_sock, &fdrd);
    FD_SET(m_sock, &fder);
    int n = select((int)(m_sock) + 1, &fdrd, NULL, &fder, &to);
    if(n > 0){
      if(FD_ISSET(m_sock, &fdrd)){
	sz = sizeof(m_sock_addr_rep);
	recvfrom(m_sock, (char*)m_trbuf, sizeof(s_tpkt),
		 0, (sockaddr*)&m_sock_addr_rep, &sz);
	rcvpkt.unpack(m_trbuf);
	if(rcvpkt.id == m_trpkt.id){
	  if(rcvpkt.del != 0){
	    // The synchronization requrest is rejected by server busy.
	    
#ifdef DEBUG_F_TIME_SYNC
	    cout << "Request denied. id: " << rcvpkt.id << " Twait: "
		 << rcvpkt.del << endl;
#endif
	    m_tnext_adj = m_cur_time + rcvpkt.del/* additional wait time */;
	    mode = SLP;
#ifdef DEBUG_F_TIME_SYNC
	    cout << "Next request is set as Tnext: " <<  m_tnext_adj << endl;
#endif
	    break;
	  }else{
	    // The server replied healthy synchronization packet.
	    
#ifdef DEBUG_F_TIME_SYNC
	    cout << "Recieved tsync packet from server id: " << rcvpkt.id 
		 << " tc1: " << rcvpkt.tc1 << " ts1: " << rcvpkt.ts1
		 << " ts2: " << rcvpkt.ts2 << " tc2: " << m_cur_time << endl;
#endif
	    m_trpkt.ts1 = rcvpkt.ts1;
	    m_trpkt.ts2 = rcvpkt.ts2;
	    m_trpkt.tc2 = get_time();
	    if(count == 0){
	      mode = FIX;
	    }else{
	      // If the error count was not zero, the delta calculated is
	      // not reliable. Try again.
	      mode = TRN;
	    }
	    break;
	  }
	}else{
	  // Pathological synchronization packet. The packet id does not match.
	  // This may happen if the server reply packet is delaied much after
	  // the client new request transmitted.
	  spdlog::error("[{}] Received packet ID {} is not match ID {} sent.",
			get_name(), rcvpkt.id, m_trpkt.id);
	  count++;
	}
      }else if(FD_ISSET(m_sock, &fder)){
	cerr << "Socket error." << endl;
	return false;
      }else{
	cerr << "Unknown error." << endl;
	return false;
      }
    }else{ // time out
      if(m_rcv_wait_count < m_max_rcv_wait_count && count == 0){
	mode = WAI;
	m_rcv_wait_count++;
      }else{
	mode = TRN;
      }
      break;
    }
  }

  return true;
}

// RCV recieves a time synchronization request. Only the first client can
// get the reply. Otherwise, last call of clearpkts() send "del" command.
// 
// State Transition
// RCV -> REP if a time syncrhonization packet is recieved.
// RCV -> TRN occurs when (1) no time synchronization request arrived,
//            and (2) the wait time passed, and (3) the filter can be a client
//            (the request's destination is specified).
// RCV -> RCV, otherwise, this occurs.
bool f_time_sync::strcv()		
{
  timeval to;
  to.tv_sec = 0;
  to.tv_usec = 10000;
  fd_set fdrd, fder;
  FD_ZERO(&fdrd);
  FD_ZERO(&fder);
  FD_SET(m_sock, &fdrd);
  FD_SET(m_sock, &fder);
  int n = select((int)(m_sock) + 1, &fdrd, NULL, &fder, &to);
  if(n > 0){
    if(FD_ISSET(m_sock, &fdrd)){
      m_sz_rep = sizeof(m_sock_addr_rep);
      recvfrom(m_sock, (char*)m_trbuf, sizeof(s_tpkt), 0,
	       (sockaddr*)&m_sock_addr_rep, &m_sz_rep);
      m_trpkt.unpack(m_trbuf);
      m_trpkt.ts1 = get_time();
#ifdef DEBUG_F_TIME_SYNC
      cout << "Tsync packet is recieved from client id: " << m_trpkt.id
	   << " tc1: " << m_trpkt.tc1 << " ts1: " << m_cur_time << endl;
#endif
      mode = REP;
    }else if(FD_ISSET(m_sock, &fder)){
      cerr << "Socket error." << endl;
      return false;
    }else{
      cerr << "Unknown error." << endl;
      return false;
    }
  }
  
  return clearpkts();
}

// REP sends a reply packet corresponding to the packet recieved in
// the RCV state.
// The packets arrived at this state are all cleared by clearpkts().
//
// State Transition
// REP -> RCV always occurs.
bool f_time_sync::strep()
{
  m_trpkt.ts2 = get_time();
  m_trpkt.del = 0;

  if(m_verb){
    spdlog::info("[{}] Replying for id {} at {}.",
		 get_name(), m_trpkt.id, m_trpkt.ts2);
  }
#ifdef DEBUG_F_TIME_SYNC
  
  cout << "Replying tsync request id: " << m_trpkt.id
       << " tc1: " << m_trpkt.tc1
       << " ts1: " << m_trpkt.ts1
       << " ts2: " << m_trpkt.ts2
       << " del: " << m_trpkt.del
       << " Size: " << sizeof(s_tpkt) << endl;
#endif

  m_trpkt.pack(m_trbuf);
  sendto(m_sock, (char*)m_trbuf, sizeof(s_tpkt), 0,
	 (sockaddr*)&m_sock_addr_rep, m_sz_rep);
  mode = RCV;
  return clearpkts();
}

// FIX corrects the time according to the time synchronization packet
// recieved at WAI state. Then the state moves to RCV.
// 
// State Transition
// FIX -> RCV always occurs.
bool f_time_sync::stfix()
{
  long long delta = m_trpkt.calc_delta();

  if(m_ch_time_sync){
    long long t = get_time();
    m_ch_time_sync->set_time_delta(t, delta);
    log.write(t, (unsigned char*)&delta, sizeof(delta)); 
  }else{
    spdlog::info("[{}] Time delay relative to server {} is {}",
		 get_name(), m_host_dst, delta);
  }

  m_tnext_adj = get_time() + (long long) m_adjust_intvl * SEC;
  mode = SLP;
  if(m_verb){
    spdlog::info("[{}] Delta fixed for id {} at {}.",
		 get_name(), m_trpkt.id, m_trpkt.tc2);
    cout << "Fix the time id: " << m_trpkt.id << " delta: " << delta << endl;
    cout << "Next request is set as Tnext: " <<  m_tnext_adj << endl;
    cout << "Move to SLP mode." << endl;
  }
  return true;
}

// SLP waits for the time next synchronization packet transmitted.
//
// State Transition
// SLP -> TRN
bool f_time_sync::stslp()
{
  if(m_tnext_adj < get_time()){
    mode = TRN;
    if(m_verb){
      spdlog::info("Current time {} > scheduled time {}. State changed to TRN"
		   , get_time(), m_tnext_adj);
    }
  }
  
  return true;
}


// Consume received packets from clients. for every client del command is
// sent with different wait time.
bool f_time_sync::clearpkts()
{
  s_tpkt rcvpkt;
  sockaddr_in addr_del;
  socklen_t len_del;
  long long del = m_adjust_intvl * SEC;
  while(1){
    timeval to;
    to.tv_sec = 0;
    to.tv_usec = 0;
    fd_set fdrd, fder;
    FD_ZERO(&fdrd);
    FD_ZERO(&fder);
    FD_SET(m_sock, &fdrd);
    FD_SET(m_sock, &fder);
    int n = select((int)(m_sock) + 1, &fdrd, NULL, &fder, &to);
    if(n > 0){
      if(FD_ISSET(m_sock, &fdrd)){
	len_del = sizeof(addr_del);
	n = recvfrom(m_sock, (char*)m_trbuf, sizeof(s_tpkt), 0,
		     (sockaddr*)&addr_del, &len_del);
	rcvpkt.unpack(m_trbuf);
	if(n == SOCKET_ERROR){
	  spdlog::error("Failed to recieve packet recvfrom() {}",
			strerror(errno));
	  return false;
	}
#ifdef DEBUG_F_TIME_SYNC
	cout << "Sending del packet id " << rcvpkt.id
	     << " Twait: " << del << endl;
#endif
	rcvpkt.del = del;
	rcvpkt.pack(m_trbuf);
	sendto(m_sock, (char*)m_trbuf, sizeof(s_tpkt), 0,
	       (sockaddr*)&addr_del, len_del);
      }else if(FD_ISSET(m_sock, &fder)){
	cerr << "Socket error." << endl;
	return false;
      }else{
	cerr << "Unknown error." << endl;
	return false;
      }
    }else{
      break;
    }
    del += m_adjust_intvl * SEC;
  }
  return true;
}
