/**
@file	 Reflector.cpp
@brief   The main reflector class
@author  Tobias Blomberg / SM0SVX
@date	 2017-02-11

\verbatim
SvxReflector - An audio reflector for connecting SvxLink Servers
Copyright (C) 2003-2017 Tobias Blomberg / SM0SVX

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
\endverbatim
*/

/****************************************************************************
 *
 * System Includes
 *
 ****************************************************************************/

#include <cassert>


/****************************************************************************
 *
 * Project Includes
 *
 ****************************************************************************/

#include <AsyncConfig.h>
#include <AsyncTcpServer.h>
#include <AsyncUdpSocket.h>
#include <AsyncApplication.h>
#include <common.h>


/****************************************************************************
 *
 * Local Includes
 *
 ****************************************************************************/

#include "Reflector.h"
#include "ReflectorClient.h"
#include "TGHandler.h"


/****************************************************************************
 *
 * Namespaces to use
 *
 ****************************************************************************/

using namespace std;
using namespace Async;



/****************************************************************************
 *
 * Defines & typedefs
 *
 ****************************************************************************/



/****************************************************************************
 *
 * Local class definitions
 *
 ****************************************************************************/



/****************************************************************************
 *
 * Prototypes
 *
 ****************************************************************************/

namespace {
void delete_client(ReflectorClient *client);
};


/****************************************************************************
 *
 * Exported Global Variables
 *
 ****************************************************************************/




/****************************************************************************
 *
 * Local Global Variables
 *
 ****************************************************************************/

namespace {
  ReflectorClient::ProtoVerRangeFilter v1_client_filter(
      ProtoVer(1, 0), ProtoVer(1, 999));
  ReflectorClient::ProtoVerRangeFilter v2_client_filter(
      ProtoVer(2, 0), ProtoVer(2, 999));
};


/****************************************************************************
 *
 * Public member functions
 *
 ****************************************************************************/

Reflector::Reflector(void)
  : m_srv(0), m_udp_sock(0), m_tg_for_v1_clients(1), m_random_qsy_lo(0),
    m_random_qsy_hi(0), m_random_qsy_tg(0), m_http_server(0)
{
  TGHandler::instance()->talkerUpdated.connect(
      mem_fun(*this, &Reflector::onTalkerUpdated));
} /* Reflector::Reflector */


Reflector::~Reflector(void)
{
  delete m_http_server;
  m_http_server = 0;
  delete m_udp_sock;
  m_udp_sock = 0;
  delete m_srv;
  m_srv = 0;

  for (ReflectorClientMap::iterator it = m_client_map.begin();
       it != m_client_map.end(); ++it)
  {
    delete (*it).second;
  }
  m_client_map.clear();

  delete TGHandler::instance();
} /* Reflector::~Reflector */


bool Reflector::initialize(Async::Config &cfg)
{
  m_cfg = &cfg;

    // Initialize the GCrypt library if not already initialized
  if (!gcry_control(GCRYCTL_INITIALIZATION_FINISHED_P))
  {
    gcry_check_version(NULL);
    gcry_error_t err;
    err = gcry_control(GCRYCTL_DISABLE_SECMEM, 0);
    if (err != GPG_ERR_NO_ERROR)
    {
      cerr << "*** ERROR: Failed to initialize the Libgcrypt library: "
           << gcry_strsource(err) << "/" << gcry_strerror(err) << endl;
      return false;
    }
      // Tell Libgcrypt that initialization has completed
    err = gcry_control(GCRYCTL_INITIALIZATION_FINISHED, 0);
    if (err != GPG_ERR_NO_ERROR)
    {
      cerr << "*** ERROR: Failed to initialize the Libgcrypt library: "
           << gcry_strsource(err) << "/" << gcry_strerror(err) << endl;
      return false;
    }
  }

  std::string listen_port("5300");
  cfg.getValue("GLOBAL", "LISTEN_PORT", listen_port);
  m_srv = new TcpServer<FramedTcpConnection>(listen_port);
  m_srv->clientConnected.connect(
      mem_fun(*this, &Reflector::clientConnected));
  m_srv->clientDisconnected.connect(
      mem_fun(*this, &Reflector::clientDisconnected));

  uint16_t udp_listen_port = 5300;
  cfg.getValue("GLOBAL", "LISTEN_PORT", udp_listen_port);
  m_udp_sock = new UdpSocket(udp_listen_port);
  if ((m_udp_sock == 0) || !m_udp_sock->initOk())
  {
    cerr << "*** ERROR: Could not initialize UDP socket" << endl;
    return false;
  }
  m_udp_sock->dataReceived.connect(
      mem_fun(*this, &Reflector::udpDatagramReceived));

  unsigned sql_timeout = 0;
  cfg.getValue("GLOBAL", "SQL_TIMEOUT", sql_timeout);
  TGHandler::instance()->setSqlTimeout(sql_timeout);

  unsigned sql_timeout_blocktime = 60;
  cfg.getValue("GLOBAL", "SQL_TIMEOUT_BLOCKTIME", sql_timeout_blocktime);
  TGHandler::instance()->setSqlTimeoutBlocktime(sql_timeout_blocktime);

  m_cfg->getValue("GLOBAL", "TG_FOR_V1_CLIENTS", m_tg_for_v1_clients);

  SvxLink::SepPair<uint32_t, uint32_t> random_qsy_range;
  if (m_cfg->getValue("GLOBAL", "RANDOM_QSY_RANGE", random_qsy_range))
  {
    m_random_qsy_lo = random_qsy_range.first;
    m_random_qsy_hi = m_random_qsy_lo + random_qsy_range.second-1;
    if ((m_random_qsy_lo < 1) || (m_random_qsy_hi < m_random_qsy_lo))
    {
      cout << "*** WARNING: Illegal RANDOM_QSY_RANGE specified. Ignored."
           << endl;
      m_random_qsy_hi = m_random_qsy_lo = 0;
    }
    m_random_qsy_tg = m_random_qsy_hi;
  }

  std::string http_srv_port;
  if (m_cfg->getValue("GLOBAL", "HTTP_SRV_PORT", http_srv_port))
  {
    m_http_server = new Async::TcpServer<Async::HttpServerConnection>(http_srv_port);
    m_http_server->clientConnected.connect(
        sigc::mem_fun(*this, &Reflector::httpClientConnected));
    m_http_server->clientDisconnected.connect(
        sigc::mem_fun(*this, &Reflector::httpClientDisconnected));
  }

  return true;
} /* Reflector::initialize */


void Reflector::nodeList(std::vector<std::string>& nodes) const
{
  nodes.clear();
  for (ReflectorClientMap::const_iterator it = m_client_map.begin();
       it != m_client_map.end(); ++it)
  {
    const std::string& callsign = (*it).second->callsign();
    if (!callsign.empty())
    {
      nodes.push_back(callsign);
    }
  }
} /* Reflector::nodeList */


void Reflector::broadcastMsg(const ReflectorMsg& msg,
                             const ReflectorClient::Filter& filter)
{
  ReflectorClientMap::const_iterator it = m_client_map.begin();
  for (; it != m_client_map.end(); ++it)
  {
    ReflectorClient *client = (*it).second;
    if (filter(client) &&
        (client->conState() == ReflectorClient::STATE_CONNECTED))
    {
      (*it).second->sendMsg(msg);
    }
  }
} /* Reflector::broadcastMsg */


bool Reflector::sendUdpDatagram(ReflectorClient *client, const void *buf,
                                size_t count)
{
  return m_udp_sock->write(client->remoteHost(), client->remoteUdpPort(), buf,
                           count);
} /* Reflector::sendUdpDatagram */


void Reflector::broadcastUdpMsg(const ReflectorUdpMsg& msg,
                                const ReflectorClient::Filter& filter)
{
  for (ReflectorClientMap::iterator it = m_client_map.begin();
       it != m_client_map.end(); ++it)
  {
    ReflectorClient *client = (*it).second;
    if (filter(client) &&
        (client->conState() == ReflectorClient::STATE_CONNECTED))
    {
      client->sendUdpMsg(msg);
    }
  }
} /* Reflector::broadcastUdpMsg */


void Reflector::requestQsy(ReflectorClient *client, uint32_t tg)
{
  if (tg == 0)
  {
    if (m_random_qsy_tg == 0)
    {
      std::cout << client->callsign() << ": QSY request for random TG "
                << "received but RANDOM_QSY_RANGE is empty" << std::endl;
      return;
    }
    uint32_t range_size = m_random_qsy_hi-m_random_qsy_lo+1;
    uint32_t i;
    for (i=0; i<range_size; ++i)
    {
      m_random_qsy_tg = (m_random_qsy_tg < m_random_qsy_hi) ?
        m_random_qsy_tg+1 : m_random_qsy_lo;
      if (TGHandler::instance()->clientsForTG(m_random_qsy_tg).empty())
      {
        tg = m_random_qsy_tg;
        break;
      }
    }
    if (i == range_size)
    {
      std::cout << "*** WARNING: No random TG available for QSY" << std::endl;
      //client->sendMsg(MsgError("No random TG available for QSY"));
      return;
    }
  }

  uint32_t current_tg = TGHandler::instance()->TGForClient(client);
  cout << client->callsign() << ": Requesting QSY from TG #"
       << current_tg << " to TG #" << tg << endl;

  broadcastMsg(MsgRequestQsy(tg),
      ReflectorClient::mkAndFilter(
        v2_client_filter,
        ReflectorClient::TgFilter(current_tg)));
} /* Reflector::requestQsy */


/****************************************************************************
 *
 * Protected member functions
 *
 ****************************************************************************/



/****************************************************************************
 *
 * Private member functions
 *
 ****************************************************************************/

void Reflector::clientConnected(Async::FramedTcpConnection *con)
{
  cout << "Client " << con->remoteHost() << ":" << con->remotePort()
       << " connected" << endl;
  ReflectorClient *rc = new ReflectorClient(this, con, m_cfg);
  m_client_map[rc->clientId()] = rc;
  m_client_con_map[con] = rc;
} /* Reflector::clientConnected */


void Reflector::clientDisconnected(Async::FramedTcpConnection *con,
                           Async::FramedTcpConnection::DisconnectReason reason)
{
  ReflectorClientConMap::iterator it = m_client_con_map.find(con);
  assert(it != m_client_con_map.end());
  ReflectorClient *client = (*it).second;

  TGHandler::instance()->removeClient(client);

  if (!client->callsign().empty())
  {
    cout << client->callsign() << ": ";
  }
  else
  {
    cout << "Client " << con->remoteHost() << ":" << con->remotePort() << " ";
  }
  cout << "disconnected: " << TcpConnection::disconnectReasonStr(reason)
       << endl;

  m_client_map.erase(client->clientId());
  m_client_con_map.erase(it);

  if (!client->callsign().empty())
  {
    broadcastMsg(MsgNodeLeft(client->callsign()),
        ReflectorClient::ExceptFilter(client));
  }
  Application::app().runTask(sigc::bind(sigc::ptr_fun(&delete_client), client));
} /* Reflector::clientDisconnected */


void Reflector::udpDatagramReceived(const IpAddress& addr, uint16_t port,
                                    void *buf, int count)
{
  stringstream ss;
  ss.write(reinterpret_cast<const char *>(buf), count);

  ReflectorUdpMsg header;
  if (!header.unpack(ss))
  {
    cout << "*** WARNING: Unpacking failed for UDP message header\n";
    return;
  }

  ReflectorClientMap::iterator it = m_client_map.find(header.clientId());
  if (it == m_client_map.end())
  {
    cerr << "*** WARNING: Incoming UDP packet has invalid client id" << endl;
    return;
  }
  ReflectorClient *client = (*it).second;
  if (addr != client->remoteHost())
  {
    cerr << "*** WARNING[" << client->callsign()
         << "]: Incoming UDP packet has the wrong source ip" << endl;
    return;
  }
  if (client->remoteUdpPort() == 0)
  {
    client->setRemoteUdpPort(port);
    client->sendUdpMsg(MsgUdpHeartbeat());
  }
  else if (port != client->remoteUdpPort())
  {
    cerr << "*** WARNING[" << client->callsign()
         << "]: Incoming UDP packet has the wrong source UDP "
            "port number" << endl;
    return;
  }

    // Check sequence number
  uint16_t udp_rx_seq_diff = header.sequenceNum() - client->nextUdpRxSeq();
  if (udp_rx_seq_diff > 0x7fff) // Frame out of sequence (ignore)
  {
    cout << client->callsign()
         << ": Dropping out of sequence frame with seq="
         << header.sequenceNum() << ". Expected seq="
         << client->nextUdpRxSeq() << endl;
    return;
  }
  else if (udp_rx_seq_diff > 0) // Frame(s) lost
  {
    cout << client->callsign()
         << ": UDP frame(s) lost. Expected seq=" << client->nextUdpRxSeq()
         << ". Received seq=" << header.sequenceNum() << endl;
  }

  client->udpMsgReceived(header);

  switch (header.type())
  {
    case MsgUdpHeartbeat::TYPE:
      break;

    case MsgUdpAudio::TYPE:
    {
      if (!client->isBlocked())
      {
        MsgUdpAudio msg;
        if (!msg.unpack(ss))
        {
          cerr << "*** WARNING[" << client->callsign()
               << "]: Could not unpack incoming MsgUdpAudioV1 message" << endl;
          return;
        }
        uint32_t tg = TGHandler::instance()->TGForClient(client);
        if (!msg.audioData().empty() && (tg > 0))
        {
          ReflectorClient* talker = TGHandler::instance()->talkerForTG(tg);
          if (talker == 0)
          {
            TGHandler::instance()->setTalkerForTG(tg, client);
            talker = TGHandler::instance()->talkerForTG(tg);
          }
          if (talker == client)
          {
            TGHandler::instance()->setTalkerForTG(tg, client);
            broadcastUdpMsg(msg,
                ReflectorClient::mkAndFilter(
                  ReflectorClient::ExceptFilter(client),
                  ReflectorClient::TgFilter(tg)));
            //broadcastUdpMsgExcept(tg, client, msg,
            //    ProtoVerRange(ProtoVer(0, 6),
            //                  ProtoVer(1, ProtoVer::max().minor())));
            //MsgUdpAudio msg_v2(msg);
            //broadcastUdpMsgExcept(tg, client, msg_v2,
            //    ProtoVerRange(ProtoVer(2, 0), ProtoVer::max()));
          }
        }
      }
      break;
    }

    //case MsgUdpAudio::TYPE:
    //{
    //  if (!client->isBlocked())
    //  {
    //    MsgUdpAudio msg;
    //    if (!msg.unpack(ss))
    //    {
    //      cerr << "*** WARNING[" << client->callsign()
    //           << "]: Could not unpack incoming MsgUdpAudio message" << endl;
    //      return;
    //    }
    //    if (!msg.audioData().empty())
    //    {
    //      if (m_talker == 0)
    //      {
    //        setTalker(client);
    //        cout << m_talker->callsign() << ": Talker start on TG #"
    //             << msg.tg() << endl;
    //      }
    //      if (m_talker == client)
    //      {
    //        gettimeofday(&m_last_talker_timestamp, NULL);
    //        broadcastUdpMsgExcept(tg, client, msg,
    //            ProtoVerRange(ProtoVer(2, 0), ProtoVer::max()));
    //        MsgUdpAudioV1 msg_v1(msg.audioData());
    //        broadcastUdpMsgExcept(tg, client, msg_v1,
    //            ProtoVerRange(ProtoVer(0, 6),
    //                          ProtoVer(1, ProtoVer::max().minor())));
    //      }
    //    }
    //  }
    //  break;
    //}

    case MsgUdpFlushSamples::TYPE:
    {
      uint32_t tg = TGHandler::instance()->TGForClient(client);
      ReflectorClient* talker = TGHandler::instance()->talkerForTG(tg);
      if ((tg > 0) && (client == talker))
      {
        TGHandler::instance()->setTalkerForTG(tg, 0);
      }
        // To be 100% correct the reflector should wait for all connected
        // clients to send a MsgUdpAllSamplesFlushed message but that will
        // probably lead to problems, especially on reflectors with many
        // clients. We therefore acknowledge the flush immediately here to
        // the client who sent the flush request.
      client->sendUdpMsg(MsgUdpAllSamplesFlushed());
      break;
    }

    case MsgUdpAllSamplesFlushed::TYPE:
      // Ignore
      break;

    default:
      // Better ignoring unknown messages to make it easier to add messages to
      // the protocol but still be backwards compatible

      //cerr << "*** WARNING[" << client->callsign()
      //     << "]: Unknown UDP protocol message received: msg_type="
      //     << header.type() << endl;
      break;
  }
} /* Reflector::udpDatagramReceived */


void Reflector::onTalkerUpdated(uint32_t tg, ReflectorClient* old_talker,
                                ReflectorClient *new_talker)
{
  if (old_talker != 0)
  {
    cout << old_talker->callsign() << ": Talker stop on TG #" << tg << endl;
    broadcastMsg(MsgTalkerStop(tg, old_talker->callsign()),
        ReflectorClient::mkAndFilter(
          v2_client_filter,
          ReflectorClient::mkOrFilter(
            ReflectorClient::TgFilter(tg),
            ReflectorClient::TgMonitorFilter(tg))));
    if (tg == tgForV1Clients())
    {
      broadcastMsg(MsgTalkerStopV1(old_talker->callsign()), v1_client_filter);
    }
    broadcastUdpMsg(MsgUdpFlushSamples(),
          ReflectorClient::mkAndFilter(
            ReflectorClient::TgFilter(tg),
            ReflectorClient::ExceptFilter(old_talker)));
  }
  if (new_talker != 0)
  {
    cout << new_talker->callsign() << ": Talker start on TG #" << tg << endl;
    broadcastMsg(MsgTalkerStart(tg, new_talker->callsign()),
        ReflectorClient::mkAndFilter(
          v2_client_filter,
          ReflectorClient::mkOrFilter(
            ReflectorClient::TgFilter(tg),
            ReflectorClient::TgMonitorFilter(tg))));
    if (tg == tgForV1Clients())
    {
      broadcastMsg(MsgTalkerStartV1(new_talker->callsign()), v1_client_filter);
    }
  }
} /* Reflector::setTalker */


void Reflector::httpRequestReceived(Async::HttpServerConnection *con,
                                    Async::HttpServerConnection::Request& req)
{
  //std::cout << "### " << req.method << " " << req.uri << std::endl;

  Async::HttpServerConnection::Response res;
  if ((req.method != "GET") && (req.method != "HEAD"))
  {
    res.setCode(501);
    res.setContent("application/json",
        "{\"msg\":\"" + req.method + ": Method not implemented\"}");
    con->write(res);
    return;
  }

  if (req.uri != "/status")
  {
    res.setCode(404);
    res.setContent("application/json",
        "{\"msg\":\"Not found!\"}");
    con->write(res);
    return;
  }

  std::ostringstream os;
  os << "{"
     //<< "\"method\":\"" << req.method << "\","
     //<< "\"uri\":\"" << req.uri << "\","
     //<< "\"headers\":{"
     ;
  //Async::HttpServerConnection::Headers::const_iterator it;
  //for (it=req.headers.begin(); it!=req.headers.end(); ++it)
  //{
  //  std::cout << (*it).first << ": " << (*it).second << std::endl;
  //  if (it != req.headers.begin())
  //  {
  //    os << ",";
  //  }
  //  os << "\"" << (*it).first << "\":\"" << (*it).second << "\"";
  //}
  //os << "},";
  ReflectorClientMap::const_iterator client_it;
  os << "\"nodes\":{";
  for (client_it = m_client_map.begin(); client_it != m_client_map.end(); ++client_it)
  {
    if (client_it != m_client_map.begin())
    {
      os << ",";
    }
    ReflectorClient* client = client_it->second;
    os << "\"" << client->callsign() << "\":{";
    os << "\"addr\":\"" << client->remoteHost() << "\",";
    os << "\"protoVer\":{\"majorVer\":" << client->protoVer().majorVer() << ",\"minorVer\":" << client->protoVer().minorVer() << "},";
    os << "\"tg\":\"" << client->currentTG() << "\",";
    os << "\"monitoredTGs\":[";
    const std::set<uint32_t>& monitored_tgs = client->monitoredTGs();
    for (std::set<uint32_t>::const_iterator mtg_it=monitored_tgs.begin();
         mtg_it!=monitored_tgs.end(); ++mtg_it)
    {
      if (mtg_it != monitored_tgs.begin())
      {
        os << ",";
      }
      os << *mtg_it;
    }
    os << "],";
    bool is_talker =
      TGHandler::instance()->talkerForTG(client->currentTG()) == client;
    os << "\"isTalker\":" << (is_talker ? "true" : "false");
    os << "}";
  }
  os << "}";
  os << "}";
  res.setContent("application/json", os.str());
  if (req.method == "HEAD")
  {
    res.setSendContent(false);
  }
  res.setCode(200);
  con->write(res);
} /* Reflector::requestReceived */


void Reflector::httpClientConnected(Async::HttpServerConnection *con)
{
  //std::cout << "### HTTP Client connected: "
  //          << con->remoteHost() << ":" << con->remotePort() << std::endl;
  con->requestReceived.connect(sigc::mem_fun(*this, &Reflector::httpRequestReceived));
} /* Reflector::httpClientConnected */


void Reflector::httpClientDisconnected(Async::HttpServerConnection *con,
    Async::HttpServerConnection::DisconnectReason reason)
{
  //std::cout << "### HTTP Client disconnected: "
  //          << con->remoteHost() << ":" << con->remotePort()
  //          << ": " << Async::HttpServerConnection::disconnectReasonStr(reason)
  //          << std::endl;
} /* Reflector::httpClientDisconnected */


namespace {
void delete_client(ReflectorClient *client) { delete client; }
};


/*
 * This file has not been truncated
 */

